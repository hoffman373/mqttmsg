/* Tests that the host client's run loop belongs to the caller
   (src/mqttmsg/transport_socket.c).

   The claim checked is that polling during the reconnect wait returns
   promptly instead of sleeping the wait out — a transport that slept would
   hold the caller's thread for up to two minutes with nothing else able to
   run, and would be untestable besides: a test that calls a function which
   does not return is a hung test. The broker address points at a closed
   port on the loopback interface, so connect() is refused immediately and
   the client goes straight into its backoff — which is the state the
   assertions are about.

   Not covered: a live session, since that needs a broker. The frames a
   session exchanges are covered by test_session.c against a fake
   transport. */

#include "unity/unity.h"

#include <mqttmsg/mqttmsg.h>
#include <mqttmsg/transport_socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Port 1. Connecting needs no privilege — only binding does — and nothing
   listens there, so the loopback stack refuses at once rather than making
   the test wait out a handshake timeout. */
#define CLOSED_PORT 1

/* Rebuilt for each test: the backoff grows with every failed attempt, so a
   shared session would hand each test whatever wait the one before it left
   behind. */
static MqttSocketTransport* wire;
static MqttClient* client;

void setUp(void) {
    wire = mqttSocketTransportNew();
    client = mqttNew();
    TEST_ASSERT_NOT_NULL(wire);
    TEST_ASSERT_NOT_NULL(client);

    mqttSocketTransportAttach(wire, client);
    mqttSetBroker(client, "127.0.0.1", CLOSED_PORT);
}

void tearDown(void) {
    mqttFree(client);
    mqttSocketTransportFree(wire);
    client = NULL;
    wire = NULL;
}

static uint64_t nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void test_aRefusedConnectionDoesNotHoldTheThread(void) {
    /* The first poll runs an attempt, has it refused, and arms the backoff.
       Every poll after that inside the wait has nothing to do and must say
       so and return: the shortest backoff is a second, so a poll that slept
       it out would turn these fifty polls into fifty seconds. */
    uint64_t start = nowMs();

    for (int i = 0; i < 50; i++) {
        mqttPoll(client);
    }

    uint64_t elapsed = nowMs() - start;
    TEST_ASSERT_LESS_THAN_UINT64(500, elapsed);
}

static void test_theFirstPollAttemptsImmediately(void) {
    /* Nothing has failed yet, so there is no wait to sit out. A client that
       armed the backoff before its first attempt would take a second to make
       one. */
    uint64_t start = nowMs();

    mqttPoll(client);

    uint64_t elapsed = nowMs() - start;
    TEST_ASSERT_LESS_THAN_UINT64(500, elapsed);
}

static void test_pollIsSafeToCallRepeatedlyAcrossTheWait(void) {
    /* The backoff grows on every failed attempt, so this walks the client
       through several attempts and several waits rather than only the first.
       Nothing here asserts timing; it is the crash and sanitizer coverage
       that matters, over the paths that open, refuse and tear down a socket
       many times. */
    for (int i = 0; i < 200; i++) {
        mqttPoll(client);
    }

    TEST_PASS();
}

/* Opens a listener on a loopback port the kernel picks, and reports which
   one. Binding to port 0 avoids the race and the privilege question that
   come with naming a port the test hopes is free. */
static int listenOnEphemeralPort(uint16_t* port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    TEST_ASSERT_EQUAL_INT(0, bind(fd, (struct sockaddr*)&addr, sizeof(addr)));
    TEST_ASSERT_EQUAL_INT(0, listen(fd, 1));

    socklen_t len = sizeof(addr);
    TEST_ASSERT_EQUAL_INT(0, getsockname(fd, (struct sockaddr*)&addr, &len));
    *port = ntohs(addr.sin_port);

    return fd;
}

static void test_aWriteToAClosedPeerFailsRatherThanKillingTheProcess(void) {
    /* The broker-restarted case: the peer goes away while the client still
       has frames to send. A send() to a socket the peer has closed raises
       SIGPIPE unless the write asks it not to, and SIGPIPE's default
       disposition ends the process — so a regression here does not fail an
       assertion, it kills this test binary outright. What must happen
       instead is that the write comes back as an error and the transport
       takes its ordinary reconnect path.

       Two writes are needed, not one: the first lands in the peer's
       already-closed socket and only earns an RST in reply, and it is the
       write after that which sees the broken pipe. */
    uint16_t port = 0;
    int listener = listenOnEphemeralPort(&port);

    mqttSetBroker(client, "127.0.0.1", port);
    mqttPoll(client);

    int peer = accept(listener, NULL, NULL);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, peer);

    /* Both ends of the broker: an open listener would let the reconnect
       succeed, which is not what this test is holding still. */
    close(peer);
    close(listener);

    /* No poll in between: a poll would see the peer's FIN, close the socket
       and take the client out of the state this test is about, so the second
       write would fail on a dead descriptor instead of a broken pipe. */
    mqttPublish(client, "t/x", "hello", false);
    mqttPublish(client, "t/x", "hello", false);
    mqttPublish(client, "t/x", "hello", false);

    mqttPoll(client);

    /* Reached only if the process is still alive. */
    TEST_ASSERT_FALSE(mqttIsConnected(client));
}

static void test_aZeroPollBudgetIsAccepted(void) {
    mqttSetPollBudget(client, 0);
    mqttPoll(client);

    /* Negative is clamped rather than handed to epoll_wait(), where it would
       mean "wait forever" and hang the caller that asked not to wait. */
    mqttSetPollBudget(client, -1);
    mqttPoll(client);

    TEST_PASS();
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_theFirstPollAttemptsImmediately);
    RUN_TEST(test_aRefusedConnectionDoesNotHoldTheThread);
    RUN_TEST(test_pollIsSafeToCallRepeatedlyAcrossTheWait);
    RUN_TEST(test_aZeroPollBudgetIsAccepted);
    RUN_TEST(test_aWriteToAClosedPeerFailsRatherThanKillingTheProcess);

    return UNITY_END();
}
