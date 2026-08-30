/* Tests for the transport-independent session layer (src/mqttmsg/session.c).

   None of this was reachable from any host build before. Both clients had
   their own copy of it, so a protocol bug had to be found and fixed twice —
   #50 survived precisely because a stalled connection is invisible to every
   gate this repo has.

   A transport is three function pointers, so a test supplies its own: an
   array that records what was written, a flag that records a close, and a
   clock the test moves by hand. That makes the sequencing assertable —
   CONNECT then SUBSCRIBE, a ping at the interval and not before, a close on
   a write that failed hard and none on one that was merely busy.

   Run under -DMQTTMSG_SANITIZE=ON for the leak half of it: every send here
   builds a frame the session is responsible for releasing. */

#include "unity/unity.h"
#include "session.h"

#include <mqttmsg/mqtt.h>
#include <mqttmsg/mqtt_types.h>
#include <string.h>

#define MAX_WRITES 16
#define MAX_FRAME 256

/* The fake transport: what was written, whether it was closed, and what
   time the test says it is. */
typedef struct {
    int writeCount;
    uint32_t lengths[MAX_WRITES];
    uint8_t bytes[MAX_WRITES][MAX_FRAME];
    int closeCount;
    uint32_t nowMs;
    /* What write() should report. Set by a test to drive the failure paths. */
    MqttWriteStatus nextStatus;
} FakeTransport;

static FakeTransport fake;

static MqttWriteStatus fakeWrite(void* ctx, const uint8_t* bytes, uint32_t length) {
    FakeTransport* t = (FakeTransport*)ctx;

    if (t->nextStatus == MqttWriteOk && t->writeCount < MAX_WRITES && length <= MAX_FRAME) {
        memcpy(t->bytes[t->writeCount], bytes, length);
        t->lengths[t->writeCount] = length;
        t->writeCount++;
    }

    return t->nextStatus;
}

static void fakeClose(void* ctx) { ((FakeTransport*)ctx)->closeCount++; }

static uint32_t fakeNowMs(void* ctx) { return ((FakeTransport*)ctx)->nowMs; }

/* The message type of the nth frame written, read straight back out of the
   bytes rather than out of anything the session remembers. */
static MessageType writtenType(int index) {
    MqttPayload pay = mqttPayloadFromBytes(fake.bytes[index], fake.lengths[index]);
    return parseFixedHeader(pay).type;
}

static MqttSession session;

void setUp(void) {
    memset(&fake, 0, sizeof(fake));
    fake.nextStatus = MqttWriteOk;

    MqttTransport transport = {
        .write = fakeWrite,
        .close = fakeClose,
        .nowMs = fakeNowMs,
        .ctx = &fake,
    };

    /* 2s keep-alive: pings at 1000ms, watchdog at 3000ms. */
    mqttSessionInit(&session, transport, 2);
}

void tearDown(void) {}

/* A CONNACK the broker would send, with the given return code in it. */
static MqttPayload makeConnAck(uint8_t returnCode) {
    static uint8_t frame[4];
    frame[0] = (uint8_t)(ConnAck << 4);
    frame[1] = 2;
    frame[2] = 0;
    frame[3] = returnCode;
    return mqttPayloadFromBytes(frame, sizeof(frame));
}

static const char* topicAt(void* ctx, int index) {
    const char* const* topics = (const char* const*)ctx;
    return topics[index];
}

/* --- message ids --------------------------------------------------- */

static void test_messageIdIsNeverZero(void) {
    /* A zero id is dropped from the serialised frame while QoS 1 is still
       declared, which desyncs the broker. */
    TEST_ASSERT_EQUAL_UINT16(1, mqttSessionNextMessageId(&session));

    session.messageId = 0xFFFF;
    TEST_ASSERT_EQUAL_UINT16(1, mqttSessionNextMessageId(&session));
}

/* --- what gets written --------------------------------------------- */

static void test_sendConnectWritesAConnect(void) {
    TEST_ASSERT_EQUAL_INT(
        MqttWriteOk, mqttSessionSendConnect(&session, "CLIENT", NULL, NULL, NULL, NULL, false));

    TEST_ASSERT_EQUAL_INT(1, fake.writeCount);
    TEST_ASSERT_EQUAL_INT(Connect, writtenType(0));
}

static void test_publishWritesAPublish(void) {
    TEST_ASSERT_EQUAL_INT(MqttWriteOk,
                          mqttSessionPublishString(&session, "a/topic", "body", false));

    TEST_ASSERT_EQUAL_INT(1, fake.writeCount);
    TEST_ASSERT_EQUAL_INT(Publish, writtenType(0));
}

static void test_subscribeWritesOneFrameForEveryTopic(void) {
    const char* topics[] = {"one/#", "two", "three"};

    TEST_ASSERT_EQUAL_INT(MqttWriteOk, mqttSessionSendSubscribe(&session, topicAt, topics, 3));

    /* One SUBSCRIBE covering all three, not three SUBSCRIBEs. */
    TEST_ASSERT_EQUAL_INT(1, fake.writeCount);
    TEST_ASSERT_EQUAL_INT(Subscribe, writtenType(0));
}

static void test_subscribeWithNothingToSendWritesNothing(void) {
    TEST_ASSERT_EQUAL_INT(MqttWriteNoFrame, mqttSessionSendSubscribe(&session, topicAt, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, fake.writeCount);
    TEST_ASSERT_EQUAL_INT(0, fake.closeCount);
}

static void test_anUnbuildableFrameIsRefusedWithoutClosing(void) {
    /* The frame never existed, so nothing is known about the connection —
       tearing it down would punish it for someone else's failed malloc. */
    MqttPayload frame = mqttPayloadFailure(PayloadFailOom);

    TEST_ASSERT_EQUAL_INT(MqttWriteNoFrame, mqttSessionSendFrame(&session, &frame, "test frame"));
    TEST_ASSERT_EQUAL_INT(0, fake.writeCount);
    TEST_ASSERT_EQUAL_INT(0, fake.closeCount);
}

/* --- CONNACK ------------------------------------------------------- */

static void test_acceptedConnAckEstablishesTheSession(void) {
    TEST_ASSERT_TRUE(mqttSessionAcceptConnAck(&session, makeConnAck(0)));
    TEST_ASSERT_TRUE(session.sessionEstablished);
}

static void test_refusedConnAckLeavesTheSessionUnestablished(void) {
    /* Reconnect pacing keys its reset off this flag, so a broker that
       refuses every CONNECT has to stay paced rather than reset each time. */
    TEST_ASSERT_FALSE(mqttSessionAcceptConnAck(&session, makeConnAck(5)));
    TEST_ASSERT_FALSE(session.sessionEstablished);
}

/* --- write failures (#50) ------------------------------------------ */

static void test_hardWriteFailureClosesTheConnection(void) {
    /* A publish that never left looks, from anywhere else, exactly like one
       that arrived. The refused write is the earlier, cheaper signal. */
    fake.nextStatus = MqttWriteFailed;

    TEST_ASSERT_EQUAL_INT(MqttWriteFailed,
                          mqttSessionPublishString(&session, "a/topic", "body", false));
    TEST_ASSERT_EQUAL_INT(1, fake.closeCount);
}

static void test_busyWriteKeepsTheConnection(void) {
    /* Back-pressure clears on its own; dropping a working session over it
       would cost more than the message that was lost. */
    fake.nextStatus = MqttWriteBusy;

    TEST_ASSERT_EQUAL_INT(MqttWriteBusy,
                          mqttSessionPublishString(&session, "a/topic", "body", false));
    TEST_ASSERT_EQUAL_INT(0, fake.closeCount);
}

static void test_failedDisconnectDoesNotClose(void) {
    /* The caller is closing anyway, and a second close counts as a second
       finished attempt against the transport's reconnect pacing. */
    fake.nextStatus = MqttWriteFailed;

    TEST_ASSERT_EQUAL_INT(MqttWriteFailed, mqttSessionSendDisconnect(&session));
    TEST_ASSERT_EQUAL_INT(0, fake.closeCount);
}

/* --- keep-alive ---------------------------------------------------- */

static void test_intervalsComeFromTheOneNumber(void) {
    /* The number the CONNECT advertises is the number the broker holds this
       client to, so the pacing has to be derived from it rather than chosen
       beside it — that is what let the host ping every 600s while promising
       90. Ping at half, give up at the broker's own one-and-a-half. */
    mqttSessionSetKeepAlive(&session, 90);
    TEST_ASSERT_EQUAL_UINT32(45000, mqttSessionPingIntervalMs(&session));
    TEST_ASSERT_EQUAL_UINT32(135000, mqttSessionSilenceTimeoutMs(&session));

    mqttSessionSetKeepAlive(&session, 0);
    TEST_ASSERT_EQUAL_UINT32(0, mqttSessionPingIntervalMs(&session));
    TEST_ASSERT_EQUAL_UINT32(0, mqttSessionSilenceTimeoutMs(&session));
}

static void test_connectAdvertisesTheKeepAlive(void) {
    /* Byte 12 and 13 of a CONNECT are the keep-alive, after the two length
       bytes, "MQIsdp", the version and the flags. */
    mqttSessionSetKeepAlive(&session, 90);
    mqttSessionSendConnect(&session, "CLIENT", NULL, NULL, NULL, NULL, false);

    TEST_ASSERT_EQUAL_INT(1, fake.writeCount);
    TEST_ASSERT_EQUAL_UINT16(90, (uint16_t)((fake.bytes[0][12] << 8) | fake.bytes[0][13]));
}

static void test_aPingAlwaysBeatsTheBrokersDeadline(void) {
    /* The whole point of halving: whatever period is chosen, a ping goes out
       with the same period again in hand before the broker may hang up. */
    for (uint16_t seconds = 1; seconds < 600; seconds++) {
        mqttSessionSetKeepAlive(&session, seconds);
        TEST_ASSERT_TRUE(mqttSessionPingIntervalMs(&session) <
                         mqttSessionSilenceTimeoutMs(&session));
    }
}

static void test_noPingBeforeTheIntervalElapses(void) {
    mqttSessionConnectionUp(&session);

    fake.nowMs = 1000; /* exactly the interval, which is not yet past it */
    TEST_ASSERT_EQUAL_INT(MqttKeepAliveIdle, mqttSessionServiceKeepAlive(&session));
    TEST_ASSERT_EQUAL_INT(0, fake.writeCount);
}

static void test_pingOnceTheIntervalElapses(void) {
    mqttSessionConnectionUp(&session);

    fake.nowMs = 1001;
    TEST_ASSERT_EQUAL_INT(MqttKeepAlivePinged, mqttSessionServiceKeepAlive(&session));
    TEST_ASSERT_EQUAL_INT(1, fake.writeCount);
    TEST_ASSERT_EQUAL_INT(PingReq, writtenType(0));

    /* The interval restarts from the ping, so the next one is a full
       interval later and not on the very next call. */
    TEST_ASSERT_EQUAL_INT(MqttKeepAliveIdle, mqttSessionServiceKeepAlive(&session));
    TEST_ASSERT_EQUAL_INT(1, fake.writeCount);
}

static void test_trafficHoldsOffTheSilenceWatchdog(void) {
    mqttSessionConnectionUp(&session);

    /* Bytes at 2500ms, so at 5000ms the broker has been silent for 2500 —
       inside the 3000ms window, even though 5000 is well past it from the
       connection coming up. */
    fake.nowMs = 2500;
    mqttSessionNoteTraffic(&session);

    fake.nowMs = 5000;
    TEST_ASSERT_EQUAL_INT(MqttKeepAlivePinged, mqttSessionServiceKeepAlive(&session));
    TEST_ASSERT_EQUAL_INT(0, fake.closeCount);
}

static void test_silenceForcesAReconnect(void) {
    /* No FIN, no RST, just a connection that stopped carrying bytes — a NAT
       or conntrack entry expiring looks exactly like this. */
    mqttSessionConnectionUp(&session);

    fake.nowMs = 3001;
    TEST_ASSERT_EQUAL_INT(MqttKeepAliveTimedOut, mqttSessionServiceKeepAlive(&session));
    TEST_ASSERT_EQUAL_INT(1, fake.closeCount);

    /* A DISCONNECT first, while the socket is still writable, so the broker
       drops the will instead of publishing it. */
    TEST_ASSERT_EQUAL_INT(1, fake.writeCount);
    TEST_ASSERT_EQUAL_INT(Disconnect, writtenType(0));
}

static void test_keepAliveOffSendsNothing(void) {
    mqttSessionSetKeepAlive(&session, 0);
    mqttSessionConnectionUp(&session);

    fake.nowMs = 1000000;
    TEST_ASSERT_EQUAL_INT(MqttKeepAliveIdle, mqttSessionServiceKeepAlive(&session));
    TEST_ASSERT_EQUAL_INT(0, fake.writeCount);
    TEST_ASSERT_EQUAL_INT(0, fake.closeCount);
}

static int beforePingCalls = 0;

static void countBeforePing(void* ctx) {
    (void)ctx;
    beforePingCalls++;
}

static void test_beforePingRunsAheadOfThePing(void) {
    beforePingCalls = 0;
    session.beforePing = countBeforePing;
    mqttSessionConnectionUp(&session);

    fake.nowMs = 1001;
    mqttSessionServiceKeepAlive(&session);
    TEST_ASSERT_EQUAL_INT(1, beforePingCalls);
}

/* --- the connection coming up -------------------------------------- */

static void test_connectionUpRestartsBothClocks(void) {
    /* A reconnect inherits neither the last connection's silence nor its
       ping schedule: it gets a full window to prove itself. */
    session.sessionEstablished = true;

    fake.nowMs = 50000;
    mqttSessionConnectionUp(&session);

    TEST_ASSERT_EQUAL_UINT32(50000, session.lastPingMs);
    TEST_ASSERT_EQUAL_UINT32(50000, session.lastRecvMs);
    TEST_ASSERT_FALSE(session.sessionEstablished);
}

/* --- client ids (#61) ---------------------------------------------- */

/* A fill that hands back a known ramp, so the mapping into letters is
   checkable rather than merely non-crashing. */
static void rampFill(void* ctx, uint8_t* out, size_t length) {
    (void)ctx;
    for (size_t i = 0; i < length; i++) {
        out[i] = (uint8_t)i;
    }
}

static void test_clientIdIsTerminatedAndFillsTheBuffer(void) {
    /* The disagreement in #61 was over exactly this: the buffer size is not
       the id length, and every caller strlen()s the result. */
    char id[8];
    memset(id, 'x', sizeof(id));

    mqttBuildClientId(id, sizeof(id), rampFill, NULL);

    TEST_ASSERT_EQUAL_size_t(7, strlen(id));
    TEST_ASSERT_EQUAL_CHAR('\0', id[7]);
}

static void test_clientIdIsUppercaseLetters(void) {
    char id[24];

    mqttBuildClientId(id, sizeof(id), rampFill, NULL);

    for (size_t i = 0; i < strlen(id); i++) {
        TEST_ASSERT_TRUE(id[i] >= 'A' && id[i] <= 'Z');
    }
}

static void test_clientIdRefusesAnEmptyBuffer(void) {
    mqttBuildClientId(NULL, 24, rampFill, NULL);
    mqttBuildClientId(NULL, 0, rampFill, NULL);
    /* Nothing to assert but that neither call wrote through a NULL. */
    TEST_PASS();
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_messageIdIsNeverZero);

    RUN_TEST(test_sendConnectWritesAConnect);
    RUN_TEST(test_publishWritesAPublish);
    RUN_TEST(test_subscribeWritesOneFrameForEveryTopic);
    RUN_TEST(test_subscribeWithNothingToSendWritesNothing);
    RUN_TEST(test_anUnbuildableFrameIsRefusedWithoutClosing);

    RUN_TEST(test_acceptedConnAckEstablishesTheSession);
    RUN_TEST(test_refusedConnAckLeavesTheSessionUnestablished);

    RUN_TEST(test_hardWriteFailureClosesTheConnection);
    RUN_TEST(test_busyWriteKeepsTheConnection);
    RUN_TEST(test_failedDisconnectDoesNotClose);

    RUN_TEST(test_intervalsComeFromTheOneNumber);
    RUN_TEST(test_connectAdvertisesTheKeepAlive);
    RUN_TEST(test_aPingAlwaysBeatsTheBrokersDeadline);
    RUN_TEST(test_noPingBeforeTheIntervalElapses);
    RUN_TEST(test_pingOnceTheIntervalElapses);
    RUN_TEST(test_trafficHoldsOffTheSilenceWatchdog);
    RUN_TEST(test_silenceForcesAReconnect);
    RUN_TEST(test_keepAliveOffSendsNothing);
    RUN_TEST(test_beforePingRunsAheadOfThePing);

    RUN_TEST(test_connectionUpRestartsBothClocks);

    RUN_TEST(test_clientIdIsTerminatedAndFillsTheBuffer);
    RUN_TEST(test_clientIdIsUppercaseLetters);
    RUN_TEST(test_clientIdRefusesAnEmptyBuffer);

    return UNITY_END();
}
