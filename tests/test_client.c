/* Tests for the client (src/mqttmsg/client.c).

   Everything here runs against transport_fake.h — arrays for the bytes and
   a clock the test winds by hand — so a whole session can be driven without
   a broker, a socket, or a wait. Subscribe, deliver a PUBLISH, assert the
   right callback ran with the right topic.

   None of this was reachable before. The client was written twice, once per
   platform, and neither copy was in a host build, so the sequencing it is
   responsible for — accept the CONNACK, then subscribe, then route what
   arrives — had no test on either. */

#include "unity/unity.h"

#include <mqttmsg/mqtt.h>
#include <mqttmsg/mqttmsg.h>
#include <mqttmsg/transport_fake.h>

#include <string.h>

#define MAX_SEEN 16

/* What the callbacks saw, so a test can assert on the sequence rather than
   on whichever one happened to run last. */
typedef struct {
    MessageType events[MAX_SEEN];
    int eventCount;
    char topics[MAX_SEEN][64];
    char bodies[MAX_SEEN][64];
    void* userData[MAX_SEEN];
    int messageCount;
} Seen;

static Seen seen;
static MqttClient* client;
static MqttFakeTransport* wire;

static void onEvent(MqttClient* c, MqttPayload frame) {
    (void)c;
    if (seen.eventCount < MAX_SEEN) {
        seen.events[seen.eventCount++] = parseFixedHeader(frame).type;
    }
}

static void onMessage(MqttClient* c, MqttString topic, MqttPayload body, void* userData) {
    (void)c;
    if (seen.messageCount >= MAX_SEEN) {
        return;
    }
    int i = seen.messageCount++;
    snprintf(seen.topics[i], sizeof(seen.topics[i]), MQTT_STR_FMT, MQTT_STR_ARG(topic));
    uint32_t n = mqttPayloadLength(body);
    if (n >= sizeof(seen.bodies[i])) {
        n = sizeof(seen.bodies[i]) - 1;
    }
    memcpy(seen.bodies[i], mqttPayloadBytes(body), n);
    seen.bodies[i][n] = '\0';
    seen.userData[i] = userData;
}

void setUp(void) {
    memset(&seen, 0, sizeof(seen));

    wire = mqttFakeTransportNew();
    client = mqttNew();
    TEST_ASSERT_NOT_NULL(wire);
    TEST_ASSERT_NOT_NULL(client);

    mqttFakeTransportAttach(wire, client);
    mqttSetBroker(client, "broker.test", 1883);

    /* The types these tests watch for. One callback records them all; which
       one fired is read back out of the frame. */
    mqttOnEvent(client, ConnAck, onEvent);
    mqttOnEvent(client, PingReq, onEvent);
    mqttOnEvent(client, Disconnect, onEvent);
}

void tearDown(void) {
    mqttFree(client);
    mqttFakeTransportFree(wire);
}

/* A CONNACK the broker would send, with the given return code. */
static MqttPayload connAck(uint8_t returnCode) {
    static uint8_t frame[4];
    frame[0] = (uint8_t)(ConnAck << 4);
    frame[1] = 2;
    frame[2] = 0;
    frame[3] = returnCode;
    return mqttPayloadFromBytes(frame, sizeof(frame));
}

/* Brings the connection up and lets the broker accept it. */
static void connectAndAccept(void) {
    mqttFakeConnect(wire);
    mqttFakeDeliver(wire, connAck(0));
    mqttPoll(client);
}

/* ── Connecting ────────────────────────────────────────────────────── */

static void test_connectingSendsAConnect(void) {
    mqttFakeConnect(wire);

    TEST_ASSERT_EQUAL_INT(1, mqttFakeWriteCount(wire));
    TEST_ASSERT_EQUAL_INT(Connect, mqttFakeWrittenType(wire, 0));
}

static void test_theConnectCarriesTheConfiguredKeepAlive(void) {
    mqttSetKeepAlive(client, 90);
    mqttFakeConnect(wire);

    /* Bytes 12 and 13 of a CONNECT are the keep-alive, after the two length
       bytes, "MQIsdp", the version and the flags. */
    MqttPayload frame = mqttFakeWritten(wire, 0);
    const uint8_t* b = mqttPayloadBytes(frame);
    TEST_ASSERT_EQUAL_UINT16(90, (uint16_t)((b[12] << 8) | b[13]));
}

static void test_aGeneratedClientIdIsStableAcrossReconnects(void) {
    /* A reconnect under a new name cannot take over the session it left
       behind, so the broker keeps the old one — subscriptions and will
       included — until it times out on its own. */
    mqttFakeConnect(wire);
    char first[32];
    snprintf(first, sizeof(first), "%s", mqttClientId(client));

    mqttFakeConnect(wire);
    TEST_ASSERT_EQUAL_STRING(first, mqttClientId(client));
    TEST_ASSERT_EQUAL_size_t(23, strlen(first));
}

static void test_anAcceptedConnAckEstablishesTheSession(void) {
    connectAndAccept();

    TEST_ASSERT_TRUE(mqttSessionIsEstablished(client));
    TEST_ASSERT_EQUAL_INT(1, seen.eventCount);
    TEST_ASSERT_EQUAL_INT(ConnAck, seen.events[0]);
}

static void test_aRefusedConnAckDoesNot(void) {
    mqttFakeConnect(wire);
    mqttFakeDeliver(wire, connAck(5));
    mqttPoll(client);

    TEST_ASSERT_FALSE(mqttSessionIsEstablished(client));
    /* The caller still hears about it — a refusal is worth reporting. */
    TEST_ASSERT_EQUAL_INT(1, seen.eventCount);
}

/* ── Subscribing ───────────────────────────────────────────────────── */

static void test_subscriptionsGoOutOnceTheBrokerAccepts(void) {
    mqttSubscribe(client, "a/#", onMessage, NULL);
    mqttSubscribe(client, "b/+", onMessage, NULL);

    connectAndAccept();

    /* One SUBSCRIBE covering both, after the CONNECT. */
    TEST_ASSERT_EQUAL_INT(2, mqttFakeWriteCount(wire));
    TEST_ASSERT_EQUAL_INT(Subscribe, mqttFakeWrittenType(wire, 1));
}

static void test_aRefusedConnAckSubscribesToNothing(void) {
    /* A SUBSCRIBE down a refused connection is a frame written into a socket
       the broker is closing. */
    mqttSubscribe(client, "a/#", onMessage, NULL);

    mqttFakeConnect(wire);
    mqttFakeDeliver(wire, connAck(5));
    mqttPoll(client);

    TEST_ASSERT_EQUAL_INT(1, mqttFakeWriteCount(wire));
    TEST_ASSERT_EQUAL_INT(Connect, mqttFakeWrittenType(wire, 0));
}

static void test_subscriptionsAreResentOnEveryConnect(void) {
    /* They do not survive a session, so a reconnect has to say them again. */
    mqttSubscribe(client, "a/#", onMessage, NULL);

    connectAndAccept();
    connectAndAccept();

    TEST_ASSERT_EQUAL_INT(4, mqttFakeWriteCount(wire));
    TEST_ASSERT_EQUAL_INT(Subscribe, mqttFakeWrittenType(wire, 3));
}

/* ── Receiving ─────────────────────────────────────────────────────── */

/* Delivers a PUBLISH and polls it through. */
static void publishTo(const char* topic, const char* body) {
    MqttPayload payload = makeStringPayload(body);
    MqttPayload frame = buildPublish(topic, 1, payload, false);
    mqttFakeDeliver(wire, frame);
    mqttPayloadFree(&payload);
    mqttPayloadFree(&frame);
    mqttPoll(client);
}

static void test_aMessageReachesTheSubscriptionThatCoversIt(void) {
    mqttSubscribe(client, "atx/+/command", onMessage, NULL);
    connectAndAccept();

    publishTo("atx/3/command", "on");

    TEST_ASSERT_EQUAL_INT(1, seen.messageCount);
    /* The topic that arrived, not the filter — which is the whole reason a
       wildcard subscription is usable. */
    TEST_ASSERT_EQUAL_STRING("atx/3/command", seen.topics[0]);
    TEST_ASSERT_EQUAL_STRING("on", seen.bodies[0]);
}

static void test_userDataDistinguishesSubscriptions(void) {
    /* Registering one filter per port with a different context each is what
       saves the callback parsing the index back out of the topic. */
    static int portOne = 1;
    static int portTwo = 2;
    mqttSubscribe(client, "atx/1/command", onMessage, &portOne);
    mqttSubscribe(client, "atx/2/command", onMessage, &portTwo);
    connectAndAccept();

    publishTo("atx/2/command", "off");

    TEST_ASSERT_EQUAL_INT(1, seen.messageCount);
    TEST_ASSERT_EQUAL_PTR(&portTwo, seen.userData[0]);
}

static void test_aMessageReachesEverySubscriptionCoveringIt(void) {
    mqttSubscribe(client, "atx/1/command", onMessage, NULL);
    mqttSubscribe(client, "other/#", onMessage, NULL);
    mqttSubscribe(client, "atx/#", onMessage, NULL);
    connectAndAccept();

    publishTo("atx/1/command", "x");

    TEST_ASSERT_EQUAL_INT(2, seen.messageCount);
}

static void test_anUnsubscribedTopicReachesNobody(void) {
    mqttSubscribe(client, "a/#", onMessage, NULL);
    connectAndAccept();

    publishTo("b/thing", "x");

    TEST_ASSERT_EQUAL_INT(0, seen.messageCount);
}

static void test_twoFramesInOneReadBothArrive(void) {
    /* TCP hands over a byte stream, not messages. */
    connectAndAccept();
    seen.eventCount = 0;

    MqttPayload ping = buildPingReq();
    MqttPayload bye = buildDisconnectMsg();
    mqttFakeDeliver(wire, ping);
    mqttFakeDeliver(wire, bye);
    mqttPayloadFree(&ping);
    mqttPayloadFree(&bye);
    mqttPoll(client);

    TEST_ASSERT_EQUAL_INT(2, seen.eventCount);
    TEST_ASSERT_EQUAL_INT(PingReq, seen.events[0]);
    TEST_ASSERT_EQUAL_INT(Disconnect, seen.events[1]);
}

/* ── Keep-alive ────────────────────────────────────────────────────── */

static void test_aPingGoesOutAtHalfTheKeepAlive(void) {
    mqttSetKeepAlive(client, 10);
    connectAndAccept();
    int before = mqttFakeWriteCount(wire);

    mqttFakeAdvanceClock(wire, 4999);
    mqttPoll(client);
    TEST_ASSERT_EQUAL_INT(before, mqttFakeWriteCount(wire));

    mqttFakeAdvanceClock(wire, 2);
    mqttPoll(client);
    TEST_ASSERT_EQUAL_INT(before + 1, mqttFakeWriteCount(wire));
    TEST_ASSERT_EQUAL_INT(PingReq, mqttFakeWrittenType(wire, before));
}

static void test_silenceForcesAReconnect(void) {
    /* No FIN, no RST, just a connection that stopped carrying bytes. */
    mqttSetKeepAlive(client, 10);
    connectAndAccept();

    mqttFakeAdvanceClock(wire, 15001);
    mqttPoll(client);

    TEST_ASSERT_EQUAL_INT(1, mqttFakeCloseCount(wire));
}

/* ── Publishing ────────────────────────────────────────────────────── */

static void test_publishWritesAPublish(void) {
    connectAndAccept();
    int before = mqttFakeWriteCount(wire);

    mqttPublish(client, "a/topic", "body", false);

    TEST_ASSERT_EQUAL_INT(before + 1, mqttFakeWriteCount(wire));
    TEST_ASSERT_EQUAL_INT(Publish, mqttFakeWrittenType(wire, before));
}

static void test_aHardWriteFailureDropsTheConnection(void) {
    /* A publish that never left looks, from anywhere else, exactly like one
       that arrived. The refused write is the earlier, cheaper signal. */
    connectAndAccept();
    mqttFakeSetWriteStatus(wire, MqttWriteFailed);

    mqttPublish(client, "a/topic", "body", false);

    TEST_ASSERT_EQUAL_INT(1, mqttFakeCloseCount(wire));
}

static void test_backPressureKeepsTheConnection(void) {
    connectAndAccept();
    mqttFakeSetWriteStatus(wire, MqttWriteBusy);

    mqttPublish(client, "a/topic", "body", false);

    TEST_ASSERT_EQUAL_INT(0, mqttFakeCloseCount(wire));
}

/* ── The run loop ─────────────────────────────────────────────────── */

/* Counts polls from inside the loop, since a run loop that never returns
   cannot be observed from outside it. */
static int idleCount;

static void countIdle(MqttClient* c, MqttPayload frame) {
    (void)c;
    (void)frame;
    idleCount++;
}

static void test_runReturnsWhenTheTransportGivesUp(void) {
    /* mqttRun() promises to return when the transport hits something it
       cannot retry. A stopped transport's poll returns immediately without
       ever disconnecting, so a loop watching only for the transport going
       away spins on it at full tilt instead — which is what this pins.

       If it regresses, this test does not fail: it hangs. That is the same
       shape as the bug. */
    idleCount = 0;
    mqttOnIdle(client, countIdle);

    mqttFakeStop(wire);
    mqttRun(client);

    /* Returned without polling at all: the transport had already given up
       before the first pass round. */
    TEST_ASSERT_EQUAL_INT(0, idleCount);
}

/* Lets the loop run a few times and then ends it, so a test can watch a
   running mqttRun() and still get its thread back. */
static void countIdleThenStop(MqttClient* c, MqttPayload frame) {
    (void)c;
    (void)frame;
    idleCount++;
    if (idleCount == 3) {
        mqttFakeStop(wire);
    }
}

static void test_runKeepsPollingATransportThatIsMerelyDisconnected(void) {
    /* The other half of the contract: a broker that is down is not something
       the transport cannot retry. It backs off and tries again, and the loop
       has to keep handing it polls rather than treating "not connected" as a
       reason to return. */
    idleCount = 0;
    mqttOnIdle(client, countIdleThenStop);

    mqttRun(client);

    TEST_ASSERT_EQUAL_INT(3, idleCount);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_connectingSendsAConnect);
    RUN_TEST(test_theConnectCarriesTheConfiguredKeepAlive);
    RUN_TEST(test_aGeneratedClientIdIsStableAcrossReconnects);
    RUN_TEST(test_anAcceptedConnAckEstablishesTheSession);
    RUN_TEST(test_aRefusedConnAckDoesNot);

    RUN_TEST(test_subscriptionsGoOutOnceTheBrokerAccepts);
    RUN_TEST(test_aRefusedConnAckSubscribesToNothing);
    RUN_TEST(test_subscriptionsAreResentOnEveryConnect);

    RUN_TEST(test_aMessageReachesTheSubscriptionThatCoversIt);
    RUN_TEST(test_userDataDistinguishesSubscriptions);
    RUN_TEST(test_aMessageReachesEverySubscriptionCoveringIt);
    RUN_TEST(test_anUnsubscribedTopicReachesNobody);
    RUN_TEST(test_twoFramesInOneReadBothArrive);

    RUN_TEST(test_aPingGoesOutAtHalfTheKeepAlive);
    RUN_TEST(test_silenceForcesAReconnect);

    RUN_TEST(test_publishWritesAPublish);
    RUN_TEST(test_aHardWriteFailureDropsTheConnection);
    RUN_TEST(test_backPressureKeepsTheConnection);

    RUN_TEST(test_runReturnsWhenTheTransportGivesUp);
    RUN_TEST(test_runKeepsPollingATransportThatIsMerelyDisconnected);

    return UNITY_END();
}
