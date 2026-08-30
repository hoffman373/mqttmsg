/* Golden frame tests: exact bytes for one frame of every type we build.

   The round-trip tests in test_mqtt.c parse what they build, so they stay
   green even if both sides drift together — a build-path change can alter
   the wire format without any of them noticing. These pin the format
   itself, so any change to the encoders has to be deliberate.

   Regenerating: these values were captured from the encoders and verified
   against a broker's expectations, not derived from the code under test.
   If one fails, the frame changed; confirm the new bytes are correct MQTT
   3.1 before updating the constant. */

#include "unity/unity.h"
#include <mqttmsg/mqtt.h>
#include <mqttmsg/mqtt_types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Compares a built frame against its expected hex, and frees it. Reports a
   readable diff rather than "expected 1 was 0" on mismatch. */
static void assertFrame(const char* what, MqttPayload actual, const char* expectedHex) {
    char got[512] = {};
    TEST_ASSERT_TRUE_MESSAGE(mqttPayloadLength(actual) * 2 < sizeof(got),
                             "frame too long for buffer");

    for (uint32_t i = 0; i < mqttPayloadLength(actual); i++) {
        snprintf(got + i * 2, 3, "%02x", mqttPayloadBytes(actual)[i]);
    }

    if (strcmp(got, expectedHex) != 0) {
        char message[1200];
        snprintf(message, sizeof(message), "%s\n  expected: %s\n  actual:   %s", what, expectedHex,
                 got);
        mqttPayloadFree(&actual);
        TEST_FAIL_MESSAGE(message);
    }

    mqttPayloadFree(&actual);
}

/* ── CONNECT ──────────────────────────────────────────────────────── */

void test_frame_connect_minimal(void) {
    assertFrame("connect-minimal", buildConnect("cid", NULL, NULL, NULL, NULL, false, 90),
                "101100064d51497364700302005a0003636964");
}

void test_frame_connect_credentials(void) {
    assertFrame("connect-credentials",
                buildConnect("cid", "alice", "s3cr3t", NULL, NULL, false, 90),
                "102000064d514973647003c2005a00036369640005616c6963650006733363723374");
}

void test_frame_connect_will(void) {
    assertFrame("connect-will", buildConnect("cid", NULL, NULL, "will/t", "gone", true, 90),
                "101f00064d5149736470032e005a0003636964000677696c6c2f740004676f6e65");
}

void test_frame_connect_full(void) {
    assertFrame("connect-full", buildConnect("myclient", "u", "p", "w/t", "bye", true, 90),
                "102600064d514973647003ee005a00086d79636c69656e740003772f740003627965"
                "000175000170");
}

/* ── PUBLISH ──────────────────────────────────────────────────────── */

/* The QoS bits and the packet identifier travel together, so the two are
   pinned as a pair: an identifier of 0 gives QoS 0 and no identifier field
   (above), a non-zero one gives QoS 1 and the two bytes that carry it. A
   QoS 1 PUBLISH with no identifier is malformed, and this library used to
   emit exactly that. */
void test_frame_publish(void) {
    MqttPayload body = makeStringPayload("sensor=42");
    MqttPayload p = buildPublish("home/temp", 0x1234, body, true);
    mqttPayloadFree(&body);
    assertFrame("publish", p, "33160009686f6d652f74656d70123473656e736f723d3432");
}

/* A zero-length topic is a present-but-empty field, not an absent one.
   Encoding it as absent gives 300178 and is what regressed in 6325879.

   Byte 0 is 0x30 rather than 0x32: a packet identifier of 0 means no
   identifier field, and a PUBLISH with no identifier is only legal at
   QoS 0. */
void test_frame_publish_empty_topic(void) {
    MqttPayload body = makeStringPayload("x");
    MqttPayload p = buildPublish("", 0, body, false);
    mqttPayloadFree(&body);
    assertFrame("publish-empty-topic", p, "3003000078");
}

/* Clearing a retained topic is a PUBLISH with a body of no bytes, so the
   frame is the header and the topic and nothing after it. An empty body that
   came back as "no body" — or as a failure, which is what malloc(0) is
   entitled to produce — would encode identically here and pass a round-trip
   test, which is why the bytes are pinned. */
void test_frame_publish_empty_retained_body(void) {
    MqttPayload body = makeStringPayload("");
    TEST_ASSERT_TRUE(mqttPayloadIsEmpty(body));
    MqttPayload p = buildPublish("t/x", 0, body, true);
    mqttPayloadFree(&body);
    assertFrame("publish-empty-retained-body", p, "31050003742f78");
}

/* ── SUBSCRIBE / UNSUBSCRIBE ──────────────────────────────────────── */

void test_frame_subscribe(void) {
    Subscription subs[2] = {
        {.topic = "a/b", .qos = AtMostOnce},
        {.topic = "c/d", .qos = AtLeastOnce},
    };
    assertFrame("subscribe", buildSubscribe(9, subs, 2), "820e00090003612f62000003632f6401");
}

void test_frame_unsubscribe(void) {
    const char* topics[2] = {"a/b", "c/d"};
    assertFrame("unsubscribe", buildUnsubscribe(0xff, topics, 2), "a20c00ff0003612f620003632f64");
}

/* ── Acks and control frames ──────────────────────────────────────── */

void test_frame_puback(void) { assertFrame("puback", buildPublishAck(0x1234), "40021234"); }

void test_frame_pubrec(void) { assertFrame("pubrec", buildPubRec(0x1234), "50021234"); }

void test_frame_pubrel(void) { assertFrame("pubrel", buildPubRel(0x1234), "62021234"); }

void test_frame_pubcomp(void) { assertFrame("pubcomp", buildPubComp(0x1234), "70021234"); }

void test_frame_pingreq(void) { assertFrame("pingreq", buildPingReq(), "c000"); }

void test_frame_disconnect(void) { assertFrame("disconnect", buildDisconnectMsg(), "e000"); }

/* ── Reserved fixed-header flags ──────────────────────────────────── */

/* The low nibble of byte 0 is DUP/QoS/RETAIN only for PUBLISH. For every
   other type those four bits are reserved: PUBREL, SUBSCRIBE and
   UNSUBSCRIBE carry 0b0010, everything else 0b0000. A receiver that sees
   anything else is entitled to drop the connection.

   The golden frames above encode this incidentally, inside nine hex
   constants. Stating it once here means a builder added later cannot get
   it wrong unnoticed — which is how PUBACK shipped as 0x42 for as long as
   it did. */
static void assertReservedFlags(const char* what, MqttPayload frame, uint8_t expected) {
    uint8_t got = mqttPayloadBytes(frame)[0] & 0x0F;
    char message[128];
    snprintf(message, sizeof(message), "%s: reserved flags 0x%X, expected 0x%X", what, got,
             expected);
    mqttPayloadFree(&frame);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(expected, got, message);
}

void test_reserved_flags_clear_where_required(void) {
    assertReservedFlags("connect", buildConnect("c", NULL, NULL, NULL, NULL, false, 90), 0x0);
    assertReservedFlags("puback", buildPublishAck(1), 0x0);
    assertReservedFlags("pubrec", buildPubRec(1), 0x0);
    assertReservedFlags("pubcomp", buildPubComp(1), 0x0);
    assertReservedFlags("pingreq", buildPingReq(), 0x0);
    assertReservedFlags("disconnect", buildDisconnectMsg(), 0x0);
}

void test_reserved_flags_set_where_required(void) {
    Subscription subs[1] = {{.topic = "a/b", .qos = AtMostOnce}};
    const char* topics[1] = {"a/b"};
    assertReservedFlags("pubrel", buildPubRel(1), 0x2);
    assertReservedFlags("subscribe", buildSubscribe(1, subs, 1), 0x2);
    assertReservedFlags("unsubscribe", buildUnsubscribe(1, topics, 1), 0x2);
}

/* PUBLISH is the exception: those bits are real fields there. */
void test_publish_flags_are_not_reserved(void) {
    MqttPayload body = makeStringPayload("x");
    MqttPayload p = buildPublish("t", 1, body, true);
    mqttPayloadFree(&body);
    uint8_t flags = mqttPayloadBytes(p)[0] & 0x0F;
    TEST_ASSERT_EQUAL_UINT8(0x1, flags & 0x1); /* retain requested */
    TEST_ASSERT_EQUAL_UINT8(AtLeastOnce, (flags >> 1) & 0x3);
    mqttPayloadFree(&p);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_frame_connect_minimal);
    RUN_TEST(test_frame_connect_credentials);
    RUN_TEST(test_frame_connect_will);
    RUN_TEST(test_frame_connect_full);

    RUN_TEST(test_frame_publish);
    RUN_TEST(test_frame_publish_empty_topic);
    RUN_TEST(test_frame_publish_empty_retained_body);

    RUN_TEST(test_frame_subscribe);
    RUN_TEST(test_frame_unsubscribe);

    RUN_TEST(test_frame_puback);
    RUN_TEST(test_frame_pubrec);
    RUN_TEST(test_frame_pubrel);
    RUN_TEST(test_frame_pubcomp);
    RUN_TEST(test_frame_pingreq);
    RUN_TEST(test_frame_disconnect);

    RUN_TEST(test_reserved_flags_clear_where_required);
    RUN_TEST(test_reserved_flags_set_where_required);
    RUN_TEST(test_publish_flags_are_not_reserved);

    return UNITY_END();
}
