#include "unity/unity.h"
#include <mqttmsg/mqtt.h>
#include <mqttmsg/mqtt_types.h>
#include <stdlib.h>
#include <string.h>

/* Message must stay the common core: smaller than the core plus even the
   smallest type-specific payload, so putting any of them back trips this.
   They belong behind an accessor, which keeps the struct that gets zeroed
   for every arriving frame small. */
_Static_assert(sizeof(Message) < sizeof(FixedHeader) + sizeof(VariableHeader) +
                                     sizeof(MqttPayload) + sizeof(MessageStatus) +
                                     sizeof(SubscriptionResponsePayload),
               "Message has grown a type-specific field");

void setUp(void) {}
void tearDown(void) {}

/* Helper: parse a built payload back into a Message. */
static Message roundtrip(MqttPayload raw) {
    FixedHeader fh = parseFixedHeader(raw);
    Message m = parseMessage(fh, raw);
    m.fixedHeader = fh;
    return m;
}

/* ── Fixed header ─────────────────────────────────────────────────── */

void test_fixedHeader_connect(void) {
    MqttPayload p = buildConnect("id", NULL, NULL, NULL, NULL, false, 90);
    FixedHeader fh = parseFixedHeader(p);
    TEST_ASSERT_EQUAL_INT(Connect, fh.type);
    TEST_ASSERT_FALSE(fh.isDup);
    TEST_ASSERT_FALSE(fh.isRetain);
    mqttPayloadFree(&p);
}

void test_fixedHeader_publish(void) {
    MqttPayload body = makeStringPayload("hello");
    MqttPayload p = buildPublish("t/x", 1, body, true);
    mqttPayloadFree(&body);
    FixedHeader fh = parseFixedHeader(p);
    TEST_ASSERT_EQUAL_INT(Publish, fh.type);
    TEST_ASSERT_TRUE(fh.isRetain);
    mqttPayloadFree(&p);
}

void test_fixedHeader_pingreq(void) {
    MqttPayload p = buildPingReq();
    FixedHeader fh = parseFixedHeader(p);
    TEST_ASSERT_EQUAL_INT(PingReq, fh.type);
    mqttPayloadFree(&p);
}

void test_fixedHeader_disconnect(void) {
    MqttPayload p = buildDisconnectMsg();
    FixedHeader fh = parseFixedHeader(p);
    TEST_ASSERT_EQUAL_INT(Disconnect, fh.type);
    mqttPayloadFree(&p);
}

/* ── CONNECT round-trip ───────────────────────────────────────────── */

void test_connect_minimal(void) {
    MqttPayload p = buildConnect("myid", NULL, NULL, NULL, NULL, false, 90);
    Message m = roundtrip(p);
    ConnectPayload cp;
    TEST_ASSERT_TRUE(mqttParseConnectPayload(&m, &cp));
    TEST_ASSERT_EQUAL_INT(Connect, m.fixedHeader.type);
    TEST_ASSERT_TRUE(mqttStrEqCStr(cp.clientId, "myid"));
    TEST_ASSERT_FALSE(m.variableHeader.connFlags.isUserName);
    TEST_ASSERT_FALSE(m.variableHeader.connFlags.isPassword);
    TEST_ASSERT_FALSE(m.variableHeader.connFlags.isWill);
    TEST_ASSERT_TRUE(m.variableHeader.connFlags.isCleanSession);
    mqttPayloadFree(&p);
}

void test_connect_with_credentials(void) {
    MqttPayload p = buildConnect("cid", "alice", "s3cr3t", NULL, NULL, false, 90);
    Message m = roundtrip(p);
    ConnectPayload cp;
    TEST_ASSERT_TRUE(mqttParseConnectPayload(&m, &cp));
    TEST_ASSERT_TRUE(mqttStrEqCStr(cp.clientId, "cid"));
    TEST_ASSERT_TRUE(mqttStrEqCStr(cp.username, "alice"));
    TEST_ASSERT_TRUE(mqttStrEqCStr(cp.password, "s3cr3t"));
    TEST_ASSERT_TRUE(m.variableHeader.connFlags.isUserName);
    TEST_ASSERT_TRUE(m.variableHeader.connFlags.isPassword);
    mqttPayloadFree(&p);
}

void test_connect_with_will(void) {
    MqttPayload p = buildConnect("cid", NULL, NULL, "will/topic", "gone", true, 90);
    Message m = roundtrip(p);
    ConnectPayload cp;
    TEST_ASSERT_TRUE(mqttParseConnectPayload(&m, &cp));
    TEST_ASSERT_TRUE(m.variableHeader.connFlags.isWill);
    TEST_ASSERT_TRUE(m.variableHeader.connFlags.isRetain);
    TEST_ASSERT_TRUE(mqttStrEqCStr(cp.willTopic, "will/topic"));
    TEST_ASSERT_TRUE(mqttStrEqCStr(cp.willMessage, "gone"));
    mqttPayloadFree(&p);
}

/* The suite had no assertion on this at all, which is how the flag stayed
   false for every CONNECT ever parsed while every round-trip test passed. */
void test_connect_protocol_name_recognised(void) {
    MqttPayload p = buildConnect("cid", NULL, NULL, NULL, NULL, false, 90);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(MessageOk, m.status);
    TEST_ASSERT_TRUE(m.variableHeader.isProtocolNameIncluded);
    mqttPayloadFree(&p);
}

void test_connect_foreign_protocol_name_rejected(void) {
    /* What buildConnect("cid", ...) emits, with the six bytes of "MQIsdp"
       replaced by "XXXXXX". Pinned rather than built and then overwritten:
       it is a parser test, so the input bytes are the fixture. */
    uint8_t raw[] = {
        0x10, 0x11,                               /* CONNECT, remaining length 17 */
        0x00, 0x06, 'X', 'X', 'X', 'X', 'X', 'X', /* not MQIsdp             */
        0x03,                                     /* version                      */
        0x02,                                     /* clean session                */
        0x00, 0x5a,                               /* keep alive 90                */
        0x00, 0x03, 'c', 'i', 'd',                /* client id                    */
    };
    MqttPayload p = mqttPayloadFromBytes(raw, sizeof(raw));
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(MessageOk, m.status);
    TEST_ASSERT_FALSE(m.variableHeader.isProtocolNameIncluded);
}

/* The rest of the variable header is located by the protocol name's actual
   length, not a hardcoded offset, so a shorter name must not desync it. */
void test_connect_shorter_protocol_name_does_not_desync(void) {
    uint8_t raw[] = {
        0x10, 0x10,                     /* CONNECT, remaining length 16 */
        0x00, 0x04, 'M', 'Q', 'T', 'T', /* 4-char name, not 6           */
        0x04,                           /* version                      */
        0x02,                           /* clean session                */
        0x00, 0x3c,                     /* keep alive 60                */
        0x00, 0x04, 'a', 'b', 'c', 'd', /* client id                    */
    };
    MqttPayload p = mqttPayloadFromBytes(raw, sizeof(raw));
    Message m = roundtrip(p);
    ConnectPayload cp;
    TEST_ASSERT_TRUE(mqttParseConnectPayload(&m, &cp));

    TEST_ASSERT_EQUAL_INT(MessageOk, m.status);
    TEST_ASSERT_FALSE(m.variableHeader.isProtocolNameIncluded); /* not MQIsdp */
    TEST_ASSERT_EQUAL_INT(4, m.variableHeader.version);
    TEST_ASSERT_EQUAL_INT(60, m.variableHeader.keepAlive);
    TEST_ASSERT_TRUE(m.variableHeader.connFlags.isCleanSession);
    TEST_ASSERT_TRUE(mqttStrEqCStr(cp.clientId, "abcd"));
}

/* Fixing the read offset without adding bounds would have unmasked an
   overread here: the frame claims a 6-byte name with fewer bytes present. */
void test_connect_truncated_protocol_name_is_invalid(void) {
    uint8_t raw[] = {0x10, 0x08, 0x00, 0x06, 'M', 'Q', 'I'};
    MqttPayload p = mqttPayloadFromBytes(raw, sizeof(raw));
    Message m = roundtrip(p);
    TEST_ASSERT_NOT_EQUAL_INT(MessageOk, m.status);
}

/* Name is complete, but the version/flags/keep-alive that follow are not. */
void test_connect_truncated_after_protocol_name_is_invalid(void) {
    uint8_t raw[] = {0x10, 0x0a, 0x00, 0x06, 'M', 'Q', 'I', 's', 'd', 'p', 0x03};
    MqttPayload p = mqttPayloadFromBytes(raw, sizeof(raw));
    Message m = roundtrip(p);
    TEST_ASSERT_NOT_EQUAL_INT(MessageOk, m.status);
}

void test_connect_keepalive(void) {
    MqttPayload p = buildConnect("x", NULL, NULL, NULL, NULL, false, 90);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(90, m.variableHeader.keepAlive);
    mqttPayloadFree(&p);
}

void test_connect_protocol_version(void) {
    MqttPayload p = buildConnect("x", NULL, NULL, NULL, NULL, false, 90);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(3, m.variableHeader.version);
    mqttPayloadFree(&p);
}

/* ── PUBLISH round-trip ───────────────────────────────────────────── */

void test_publish_topic_and_payload(void) {
    MqttPayload body = makeStringPayload("sensor=42");
    MqttPayload p = buildPublish("home/temp", 7, body, false);
    mqttPayloadFree(&body);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(Publish, m.fixedHeader.type);
    TEST_ASSERT_TRUE(mqttStrEqCStr(m.variableHeader.topicName, "home/temp"));
    TEST_ASSERT_EQUAL_UINT32(9, mqttPayloadLength(m.payload));
    TEST_ASSERT_EQUAL_INT(0, strncmp((const char*)mqttPayloadBytes(m.payload), "sensor=42", 9));
    mqttPayloadFree(&p);
}

void test_publish_message_id(void) {
    MqttPayload body = makeStringPayload("x");
    MqttPayload p = buildPublish("t", 0x0102, body, false);
    mqttPayloadFree(&body);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_UINT16(0x0102, m.variableHeader.messageId);
    mqttPayloadFree(&p);
}

void test_publish_message_id_high(void) {
    MqttPayload body = makeStringPayload("x");
    MqttPayload p = buildPublish("t", 0xFF00, body, false);
    mqttPayloadFree(&body);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_UINT16(0xFF00, m.variableHeader.messageId);
    mqttPayloadFree(&p);
}

void test_publish_without_a_message_id_is_qos0(void) {
    /* writeVarHeader() emits the packet identifier only when it is non-zero,
       so a builder that always claimed QoS 1 produced a QoS 1 PUBLISH with no
       identifier — malformed, and the parser is right to refuse it. The QoS
       bits follow the identifier instead, which keeps the two consistent.

       The body matters here: at QoS 1 the parser would take "hi" for the
       identifier and leave no body at all. */
    MqttPayload body = makeStringPayload("hi");
    MqttPayload p = buildPublish("t", 0, body, false);
    mqttPayloadFree(&body);

    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(AtMostOnce, m.fixedHeader.qos);
    TEST_ASSERT_EQUAL_INT(MessageOk, m.status);
    TEST_ASSERT_EQUAL_UINT16(0, m.variableHeader.messageId);
    TEST_ASSERT_EQUAL_UINT32(2, mqttPayloadLength(m.payload));
    TEST_ASSERT_EQUAL_INT(0, memcmp(mqttPayloadBytes(m.payload), "hi", 2));

    mqttPayloadFree(&p);
}

void test_publish_with_a_message_id_is_qos1(void) {
    /* The other half of the coupling: an identifier to carry means the frame
       has to be at a QoS that is allowed to carry one. */
    MqttPayload body = makeStringPayload("hi");
    MqttPayload p = buildPublish("t", 9, body, false);
    mqttPayloadFree(&body);

    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(AtLeastOnce, m.fixedHeader.qos);
    TEST_ASSERT_EQUAL_INT(MessageOk, m.status);
    TEST_ASSERT_EQUAL_UINT16(9, m.variableHeader.messageId);
    TEST_ASSERT_EQUAL_UINT32(2, mqttPayloadLength(m.payload));

    mqttPayloadFree(&p);
}

void test_publish_retain_flag(void) {
    MqttPayload body = makeStringPayload("v");
    MqttPayload p = buildPublish("t", 1, body, true);
    mqttPayloadFree(&body);
    FixedHeader fh = parseFixedHeader(p);
    TEST_ASSERT_TRUE(fh.isRetain);
    mqttPayloadFree(&p);
}

/* ── PUBACK round-trip ────────────────────────────────────────────── */

void test_puback_message_id(void) {
    MqttPayload p = buildPublishAck(0x1234);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(PubAck, m.fixedHeader.type);
    TEST_ASSERT_EQUAL_UINT16(0x1234, m.variableHeader.messageId);
    mqttPayloadFree(&p);
}

/* ── PUBREC round-trip ────────────────────────────────────────────── */

void test_pubrec_type(void) {
    MqttPayload p = buildPubRec(0x0001);
    FixedHeader fh = parseFixedHeader(p);
    TEST_ASSERT_EQUAL_INT(PubRec, fh.type);
    mqttPayloadFree(&p);
}

void test_pubrec_message_id(void) {
    MqttPayload p = buildPubRec(0x1234);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(PubRec, m.fixedHeader.type);
    TEST_ASSERT_EQUAL_UINT16(0x1234, m.variableHeader.messageId);
    mqttPayloadFree(&p);
}

void test_pubrec_message_id_high(void) {
    MqttPayload p = buildPubRec(0xFF00);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_UINT16(0xFF00, m.variableHeader.messageId);
    mqttPayloadFree(&p);
}

void test_pubrec_no_payload(void) {
    MqttPayload p = buildPubRec(0x0001);
    Message m = roundtrip(p);
    /* No body at all, which is a different thing from an empty one. */
    TEST_ASSERT_TRUE(mqttPayloadIsNone(m.payload));
    TEST_ASSERT_FALSE(mqttPayloadIsEmpty(m.payload));
    mqttPayloadFree(&p);
}

/* ── PUBREL round-trip ────────────────────────────────────────────── */

void test_pubrel_type(void) {
    MqttPayload p = buildPubRel(0x0001);
    FixedHeader fh = parseFixedHeader(p);
    TEST_ASSERT_EQUAL_INT(PubRel, fh.type);
    mqttPayloadFree(&p);
}

void test_pubrel_qos_flag(void) {
    MqttPayload p = buildPubRel(0x0001);
    FixedHeader fh = parseFixedHeader(p);
    TEST_ASSERT_EQUAL_INT(AtLeastOnce, fh.qos);
    mqttPayloadFree(&p);
}

void test_pubrel_message_id(void) {
    MqttPayload p = buildPubRel(0x1234);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(PubRel, m.fixedHeader.type);
    TEST_ASSERT_EQUAL_UINT16(0x1234, m.variableHeader.messageId);
    mqttPayloadFree(&p);
}

void test_pubrel_message_id_high(void) {
    MqttPayload p = buildPubRel(0xFF00);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_UINT16(0xFF00, m.variableHeader.messageId);
    mqttPayloadFree(&p);
}

void test_pubrel_no_payload(void) {
    MqttPayload p = buildPubRel(0x0001);
    Message m = roundtrip(p);
    /* No body at all, which is a different thing from an empty one. */
    TEST_ASSERT_TRUE(mqttPayloadIsNone(m.payload));
    TEST_ASSERT_FALSE(mqttPayloadIsEmpty(m.payload));
    mqttPayloadFree(&p);
}

/* ── PUBCOMP round-trip ───────────────────────────────────────────── */

void test_pubcomp_type(void) {
    MqttPayload p = buildPubComp(0x0001);
    FixedHeader fh = parseFixedHeader(p);
    TEST_ASSERT_EQUAL_INT(PubComp, fh.type);
    mqttPayloadFree(&p);
}

void test_pubcomp_message_id(void) {
    MqttPayload p = buildPubComp(0x1234);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(PubComp, m.fixedHeader.type);
    TEST_ASSERT_EQUAL_UINT16(0x1234, m.variableHeader.messageId);
    mqttPayloadFree(&p);
}

void test_pubcomp_message_id_high(void) {
    MqttPayload p = buildPubComp(0xFF00);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_UINT16(0xFF00, m.variableHeader.messageId);
    mqttPayloadFree(&p);
}

void test_pubcomp_no_payload(void) {
    MqttPayload p = buildPubComp(0x0001);
    Message m = roundtrip(p);
    /* No body at all, which is a different thing from an empty one. */
    TEST_ASSERT_TRUE(mqttPayloadIsNone(m.payload));
    TEST_ASSERT_FALSE(mqttPayloadIsEmpty(m.payload));
    mqttPayloadFree(&p);
}

/* ── SUBSCRIBE round-trip ─────────────────────────────────────────── */

void test_subscribe_single_topic(void) {
    Subscription subs[1] = {{.topic = "a/b", .qos = AtLeastOnce}};
    MqttPayload p = buildSubscribe(5, subs, 1);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(Subscribe, m.fixedHeader.type);
    TEST_ASSERT_EQUAL_UINT16(5, m.variableHeader.messageId);

    SubscriptionPayload sp;
    TEST_ASSERT_TRUE(mqttParseSubscriptions(&m, &sp));
    TEST_ASSERT_EQUAL_INT(1, sp.subscriptionCount);

    MqttSubscriptionCursor cursor = sp.cursor;
    MqttSubscription got;
    TEST_ASSERT_TRUE(mqttNextSubscription(&cursor, &got));
    TEST_ASSERT_TRUE(mqttStrEqCStr(got.topic, "a/b"));
    TEST_ASSERT_EQUAL_INT(AtLeastOnce, got.qos);
    TEST_ASSERT_FALSE(mqttNextSubscription(&cursor, &got));
    mqttPayloadFree(&p);
}

void test_subscribe_multiple_topics(void) {
    Subscription subs[2] = {
        {.topic = "a/b", .qos = AtMostOnce},
        {.topic = "c/d", .qos = AtLeastOnce},
    };
    MqttPayload p = buildSubscribe(9, subs, 2);
    Message m = roundtrip(p);

    SubscriptionPayload sp;
    TEST_ASSERT_TRUE(mqttParseSubscriptions(&m, &sp));
    TEST_ASSERT_EQUAL_INT(2, sp.subscriptionCount);

    MqttSubscriptionCursor cursor = sp.cursor;
    MqttSubscription got;
    TEST_ASSERT_TRUE(mqttNextSubscription(&cursor, &got));
    TEST_ASSERT_TRUE(mqttStrEqCStr(got.topic, "a/b"));
    TEST_ASSERT_EQUAL_INT(AtMostOnce, got.qos);
    TEST_ASSERT_TRUE(mqttNextSubscription(&cursor, &got));
    TEST_ASSERT_TRUE(mqttStrEqCStr(got.topic, "c/d"));
    TEST_ASSERT_EQUAL_INT(AtLeastOnce, got.qos);
    TEST_ASSERT_FALSE(mqttNextSubscription(&cursor, &got));
    mqttPayloadFree(&p);
}

void test_subscribe_message_id(void) {
    Subscription subs[1] = {{.topic = "x", .qos = AtMostOnce}};
    MqttPayload p = buildSubscribe(0xABCD, subs, 1);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_UINT16(0xABCD, m.variableHeader.messageId);
    mqttPayloadFree(&p);
}

/* ── UNSUBSCRIBE round-trip ───────────────────────────────────────── */

void test_unsubscribe_message_id(void) {
    const char* topics[] = {"a/b", "c/d"};
    MqttPayload p = buildUnsubscribe(0x00FF, topics, 2);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(Unsubscribe, m.fixedHeader.type);
    TEST_ASSERT_EQUAL_UINT16(0x00FF, m.variableHeader.messageId);
    mqttPayloadFree(&p);
}

/* A zero-length topic is a field that is present and empty, not an absent
   field. The build path must test MqttString.data for presence rather than
   .length for emptiness, or the topic silently vanishes from the frame. */
void test_publish_empty_topic_keeps_topic_field(void) {
    MqttPayload body = makeStringPayload("x");
    MqttPayload p = buildPublish("", 0, body, false);
    mqttPayloadFree(&body);

    /* 32 03 | 00 00 | 78 — dropping the field would give 32 01 78. */
    TEST_ASSERT_EQUAL_UINT8(3, mqttPayloadBytes(p)[1]);
    TEST_ASSERT_EQUAL_UINT8(0, mqttPayloadBytes(p)[2]);
    TEST_ASSERT_EQUAL_UINT8(0, mqttPayloadBytes(p)[3]);
    mqttPayloadFree(&p);
}

/* ── Accessors refuse the wrong type ──────────────────────────────── */

/* The reason the type-specific fields live behind accessors: asking a
   message for fields it does not have is refused, rather than answered
   with a silently zeroed struct. */
void test_accessors_reject_wrong_type(void) {
    MqttPayload body = makeStringPayload("v");
    MqttPayload p = buildPublish("t/x", 1, body, false);
    mqttPayloadFree(&body);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(MessageOk, m.status);

    ConnectPayload cp;
    SubscriptionPayload sp;
    UnsubscribePayload up;
    SubscriptionResponsePayload sr;
    TEST_ASSERT_FALSE(mqttParseConnectPayload(&m, &cp));
    TEST_ASSERT_FALSE(mqttParseSubscriptions(&m, &sp));
    TEST_ASSERT_FALSE(mqttParseUnsubTopics(&m, &up));
    TEST_ASSERT_FALSE(mqttParseGrantedQos(&m, &sr));
    mqttPayloadFree(&p);
}

void test_accessors_clear_their_output_on_refusal(void) {
    MqttPayload p = buildPingReq();
    Message m = roundtrip(p);

    ConnectPayload cp;
    memset(&cp, 0xEE, sizeof(cp));
    TEST_ASSERT_FALSE(mqttParseConnectPayload(&m, &cp));
    /* Refused means the out-param is safe to look at, not left as garbage. */
    TEST_ASSERT_FALSE(mqttStrIsPresent(cp.clientId));
    mqttPayloadFree(&p);
}

void test_accessors_reject_null_arguments(void) {
    MqttPayload p = buildConnect("cid", NULL, NULL, NULL, NULL, false, 90);
    Message m = roundtrip(p);
    ConnectPayload cp;
    TEST_ASSERT_FALSE(mqttParseConnectPayload(NULL, &cp));
    TEST_ASSERT_FALSE(mqttParseConnectPayload(&m, NULL));
    mqttPayloadFree(&p);
}

void test_accessors_reject_unusable_message(void) {
    /* A frame that did not parse must not hand out fields either. */
    MqttPayload p = buildConnect("cid", "alice", "s3cr3t", NULL, NULL, false, 90);
    FixedHeader fh = parseFixedHeader(p);
    MqttPayload truncated = mqttPayloadPrefix(p, 16);
    Message m = parseMessage(fh, truncated);

    TEST_ASSERT_NOT_EQUAL_INT(MessageOk, m.status);
    ConnectPayload cp;
    TEST_ASSERT_FALSE(mqttParseConnectPayload(&m, &cp));
    mqttPayloadFree(&p);
}

void test_accessor_can_be_called_repeatedly(void) {
    /* Nothing is consumed by reading, so the same message answers twice. */
    MqttPayload p = buildConnect("cid", NULL, NULL, NULL, NULL, false, 90);
    Message m = roundtrip(p);

    ConnectPayload first;
    ConnectPayload second;
    TEST_ASSERT_TRUE(mqttParseConnectPayload(&m, &first));
    TEST_ASSERT_TRUE(mqttParseConnectPayload(&m, &second));
    TEST_ASSERT_TRUE(mqttStrEq(first.clientId, second.clientId));
    mqttPayloadFree(&p);
}

/* ── Truncated versus malformed ───────────────────────────────────── */

/* A stream reader needs to tell "wait for more bytes" from "drop the
   connection", which a single valid/invalid flag could not express. */
void test_short_read_is_incomplete(void) {
    MqttPayload body = makeStringPayload("sensor=42");
    MqttPayload p = buildPublish("home/temp", 1, body, false);
    mqttPayloadFree(&body);

    FixedHeader fh = parseFixedHeader(p);
    MqttPayload partial = mqttPayloadPrefix(p, 6);
    Message m = parseMessage(fh, partial);

    /* The frame is fine as far as it goes; the rest simply has not arrived. */
    TEST_ASSERT_EQUAL_INT(MessageIncomplete, m.status);
    mqttPayloadFree(&p);
}

void test_self_contradicting_frame_is_malformed(void) {
    /* Remaining length says 4 bytes follow, but the topic inside claims 40.
       No amount of waiting fixes that. */
    uint8_t raw[] = {0x30, 0x04, 0x00, 0x28, 'a', 'b'};
    MqttPayload p = mqttPayloadFromBytes(raw, sizeof(raw));
    FixedHeader fh = parseFixedHeader(p);
    Message m = parseMessage(fh, p);
    TEST_ASSERT_EQUAL_INT(MessageMalformed, m.status);
}

/* ── Zero-copy contract ───────────────────────────────────────────── */

/* The whole point of the change: nothing is copied out of the frame. If
   parsing ever goes back to allocating, these fail. */
static bool pointsInside(const void* p, MqttPayload frame) {
    const uint8_t* at = (const uint8_t*)p;
    return at >= mqttPayloadBytes(frame) && at < mqttPayloadBytes(frame) + mqttPayloadLength(frame);
}

void test_publish_topic_is_a_view(void) {
    MqttPayload body = makeStringPayload("sensor=42");
    MqttPayload p = buildPublish("home/temp", 1, body, false);
    mqttPayloadFree(&body);
    Message m = roundtrip(p);
    TEST_ASSERT_TRUE(mqttStrEqCStr(m.variableHeader.topicName, "home/temp"));
    TEST_ASSERT_TRUE(pointsInside(m.variableHeader.topicName.data, p));
    mqttPayloadFree(&p);
}

void test_connect_strings_are_views(void) {
    MqttPayload p = buildConnect("cid", "alice", "s3cr3t", NULL, NULL, false, 90);
    Message m = roundtrip(p);
    ConnectPayload cp;
    TEST_ASSERT_TRUE(mqttParseConnectPayload(&m, &cp));
    TEST_ASSERT_TRUE(pointsInside(cp.clientId.data, p));
    TEST_ASSERT_TRUE(pointsInside(cp.username.data, p));
    TEST_ASSERT_TRUE(pointsInside(cp.password.data, p));
    mqttPayloadFree(&p);
}

void test_subscribe_topics_are_views(void) {
    Subscription subs[1] = {{.topic = "a/b", .qos = AtMostOnce}};
    MqttPayload p = buildSubscribe(1, subs, 1);
    Message m = roundtrip(p);
    SubscriptionPayload sp;
    TEST_ASSERT_TRUE(mqttParseSubscriptions(&m, &sp));
    MqttSubscriptionCursor cursor = sp.cursor;
    MqttSubscription got;
    TEST_ASSERT_TRUE(mqttNextSubscription(&cursor, &got));
    TEST_ASSERT_TRUE(pointsInside(got.topic.data, p));
    mqttPayloadFree(&p);
}

void test_cursor_can_be_walked_twice(void) {
    Subscription subs[2] = {
        {.topic = "a/b", .qos = AtMostOnce},
        {.topic = "c/d", .qos = AtLeastOnce},
    };
    MqttPayload p = buildSubscribe(1, subs, 2);
    Message m = roundtrip(p);

    /* The cursor in the Message is never advanced, so copying it yields a
       fresh walk every time. */
    for (int pass = 0; pass < 2; pass++) {
        SubscriptionPayload sp;
        TEST_ASSERT_TRUE(mqttParseSubscriptions(&m, &sp));
        MqttSubscriptionCursor cursor = sp.cursor;
        MqttSubscription got;
        int count = 0;
        while (mqttNextSubscription(&cursor, &got)) {
            count++;
        }
        TEST_ASSERT_EQUAL_INT(2, count);
    }

    mqttPayloadFree(&p);
}

/* ── Malformed frames ─────────────────────────────────────────────── */

void test_well_formed_message_is_valid(void) {
    MqttPayload body = makeStringPayload("v");
    MqttPayload p = buildPublish("t/x", 1, body, false);
    mqttPayloadFree(&body);
    Message m = roundtrip(p);
    TEST_ASSERT_EQUAL_INT(MessageOk, m.status);
    mqttPayloadFree(&p);
}

void test_truncated_publish_topic_is_invalid(void) {
    /* A frame that claims a longer topic than actually arrived — what a
       short read produces. Parsing it must refuse rather than read past
       the end of the buffer. */
    MqttPayload body = makeStringPayload("sensor=42");
    MqttPayload p = buildPublish("home/temp", 1, body, false);
    mqttPayloadFree(&body);

    FixedHeader fh = parseFixedHeader(p);
    MqttPayload truncated = mqttPayloadPrefix(p, 6);
    Message m = parseMessage(fh, truncated);

    TEST_ASSERT_NOT_EQUAL_INT(MessageOk, m.status);
    TEST_ASSERT_TRUE(mqttStrIsEmpty(m.variableHeader.topicName));
    mqttPayloadFree(&p);
}

void test_truncated_connect_is_invalid(void) {
    MqttPayload p = buildConnect("myclient", "alice", "s3cr3t", NULL, NULL, false, 90);
    FixedHeader fh = parseFixedHeader(p);
    MqttPayload truncated = mqttPayloadPrefix(p, 16);
    Message m = parseMessage(fh, truncated);
    TEST_ASSERT_NOT_EQUAL_INT(MessageOk, m.status);
    mqttPayloadFree(&p);
}

void test_truncated_subscribe_is_invalid(void) {
    Subscription subs[2] = {
        {.topic = "a/b", .qos = AtMostOnce},
        {.topic = "c/d", .qos = AtLeastOnce},
    };
    MqttPayload p = buildSubscribe(1, subs, 2);
    FixedHeader fh = parseFixedHeader(p);
    /* Cut inside the second entry. */
    MqttPayload truncated = mqttPayloadPrefix(p, mqttPayloadLength(p) - 3);
    Message m = parseMessage(fh, truncated);
    TEST_ASSERT_NOT_EQUAL_INT(MessageOk, m.status);
    mqttPayloadFree(&p);
}

/* ── Fixed-offset reads past the frame end (#85 regression) ───────────── */

/* The topic reads were bounds-checked, but the fields read at a fixed offset
   after them were not: a frame whose remaining-length field stops short of the
   message id or the CONNACK return code was accepted as MessageOk, read past
   its own end, and — because pos then overran the length — reported a payload
   of roughly four billion bytes that reached user callbacks. Each of these
   frames is exactly what the framer would hand parseMessage(): the bytes the
   remaining-length field claims, no more. They must be refused, and must leave
   no oversized payload behind.

   A whole-buffer sweep by AddressSanitizer over these inputs was the original
   reproducer; these assertions pin the same cases without needing ASan to run. */

void test_publish_qos1_without_message_id_is_rejected(void) {
    /* Remaining length 7 covers the topic "hi/tx" (2 + 5) exactly, with no
       room for the two-byte message id the QoS-1 flag promises. */
    uint8_t raw[] = {0x32, 0x07, 0x00, 0x05, 'h', 'i', '/', 't', 'x'};
    MqttPayload p = mqttPayloadFromBytes(raw, sizeof(raw));
    Message m = parseMessage(parseFixedHeader(p), p);
    TEST_ASSERT_EQUAL_INT(MessageMalformed, m.status);
    /* The tell-tale of the old bug: a wrapped length near UINT32_MAX. */
    TEST_ASSERT_EQUAL_UINT32(0, mqttPayloadLength(m.payload));
}

void test_suback_without_message_id_is_rejected(void) {
    /* Remaining length 1: the message id needs two bytes. */
    uint8_t raw[] = {0x90, 0x01, 0x00};
    MqttPayload p = mqttPayloadFromBytes(raw, sizeof(raw));
    Message m = parseMessage(parseFixedHeader(p), p);
    TEST_ASSERT_EQUAL_INT(MessageMalformed, m.status);
    TEST_ASSERT_EQUAL_UINT32(0, mqttPayloadLength(m.payload));
}

void test_puback_without_message_id_is_rejected(void) {
    /* Remaining length 0: a PUBACK still owes a two-byte message id. */
    uint8_t raw[] = {0x40, 0x00};
    MqttPayload p = mqttPayloadFromBytes(raw, sizeof(raw));
    Message m = parseMessage(parseFixedHeader(p), p);
    TEST_ASSERT_EQUAL_INT(MessageMalformed, m.status);
    TEST_ASSERT_EQUAL_UINT32(0, mqttPayloadLength(m.payload));
}

void test_connack_without_return_code_is_rejected(void) {
    /* Remaining length 0: the two-byte CONNACK variable header is absent, so
       the return code would be read from past the frame. */
    uint8_t raw[] = {0x20, 0x00};
    MqttPayload p = mqttPayloadFromBytes(raw, sizeof(raw));
    Message m = parseMessage(parseFixedHeader(p), p);
    TEST_ASSERT_EQUAL_INT(MessageMalformed, m.status);
}

/* ── SUBACK granted QoS ───────────────────────────────────────────── */

void test_suback_granted_qos(void) {
    /* SUBACK: fixed header, remaining length, message id, then one granted
       QoS byte per subscription. */
    uint8_t raw[] = {0x90, 0x04, 0x00, 0x01, 0x00, 0x02};
    MqttPayload p = mqttPayloadFromBytes(raw, sizeof(raw));
    Message m = roundtrip(p);
    SubscriptionResponsePayload sr;
    TEST_ASSERT_TRUE(mqttParseGrantedQos(&m, &sr));

    TEST_ASSERT_EQUAL_INT(MessageOk, m.status);
    TEST_ASSERT_EQUAL_INT(2, sr.grantedCount);
    TEST_ASSERT_EQUAL_INT(AtMostOnce, mqttGrantedQosAt(sr, 0));
    TEST_ASSERT_EQUAL_INT(ExactlyOnce, mqttGrantedQosAt(sr, 1));
    /* Out of range is clamped rather than read past the buffer. */
    TEST_ASSERT_EQUAL_INT(AtMostOnce, mqttGrantedQosAt(sr, 99));
    TEST_ASSERT_EQUAL_INT(AtMostOnce, mqttGrantedQosAt(sr, -1));
}

void test_suback_granted_qos_is_a_view(void) {
    uint8_t raw[] = {0x90, 0x03, 0x00, 0x01, 0x01};
    MqttPayload p = mqttPayloadFromBytes(raw, sizeof(raw));
    Message m = roundtrip(p);
    SubscriptionResponsePayload sr;
    TEST_ASSERT_TRUE(mqttParseGrantedQos(&m, &sr));
    TEST_ASSERT_TRUE(pointsInside(sr.grantedQos, p));
}

/* ── UNSUBSCRIBE cursor ───────────────────────────────────────────── */

void test_unsubscribe_topics(void) {
    const char* topics[] = {"a/b", "c/d"};
    MqttPayload p = buildUnsubscribe(0x00FF, topics, 2);
    Message m = roundtrip(p);

    TEST_ASSERT_EQUAL_INT(MessageOk, m.status);

    UnsubscribePayload up;
    TEST_ASSERT_TRUE(mqttParseUnsubTopics(&m, &up));
    TEST_ASSERT_EQUAL_INT(2, up.topicCount);

    MqttTopicCursor cursor = up.cursor;
    MqttString topic;
    TEST_ASSERT_TRUE(mqttNextUnsubTopic(&cursor, &topic));
    TEST_ASSERT_TRUE(mqttStrEqCStr(topic, "a/b"));
    TEST_ASSERT_TRUE(pointsInside(topic.data, p));
    TEST_ASSERT_TRUE(mqttNextUnsubTopic(&cursor, &topic));
    TEST_ASSERT_TRUE(mqttStrEqCStr(topic, "c/d"));
    TEST_ASSERT_FALSE(mqttNextUnsubTopic(&cursor, &topic));

    mqttPayloadFree(&p);
}

/* ── mqttMakeString / makeStringPayload ───────────────────────────────── */

void test_makeString_length_prefix(void) {
    MqttPayload p = mqttMakeString("hello");
    TEST_ASSERT_EQUAL_INT(7, mqttPayloadLength(p)); /* 2-byte length + 5 chars */
    TEST_ASSERT_EQUAL_UINT8(0x00, mqttPayloadBytes(p)[0]);
    TEST_ASSERT_EQUAL_UINT8(0x05, mqttPayloadBytes(p)[1]);
    TEST_ASSERT_EQUAL_INT(0, memcmp(mqttPayloadBytes(p) + 2, "hello", 5));
    mqttPayloadFree(&p);
}

void test_makeStringPayload_no_prefix(void) {
    MqttPayload p = makeStringPayload("hi");
    TEST_ASSERT_EQUAL_INT(2, mqttPayloadLength(p));
    TEST_ASSERT_EQUAL_INT(0, memcmp(mqttPayloadBytes(p), "hi", 2));
    mqttPayloadFree(&p);
}

/* ── Frame length limit ───────────────────────────────────────────── */

/* MQTT's remaining-length field is four bytes of seven bits, so 268435455 is
   the longest frame the protocol can describe. The encoder's buffer is sized
   from the same four-byte cap, which is why a longer frame has to be refused
   rather than encoded: the encode loop would run to a fifth byte and write
   one past the buffer.

   The body is calloc'd and never read — the builder rejects the frame before
   copying anything — so this costs address space rather than resident
   memory. */
void test_frame_longer_than_the_protocol_allows_is_refused(void) {
    const uint32_t maxRemaining = 268435455u;

    uint8_t* body = calloc(maxRemaining, 1);
    if (body == NULL) {
        TEST_IGNORE_MESSAGE("not enough address space for the over-limit body");
    }

    /* The body alone is at the limit, so the topic and packet identifier in
       front of it push the frame past it. */
    MqttPayload huge = mqttPayloadFromBytes(body, maxRemaining);
    MqttPayload frame = buildPublish("t", 1, huge, false);

    TEST_ASSERT_TRUE(mqttPayloadIsFailure(frame));
    TEST_ASSERT_EQUAL_INT(PayloadFailBadArgs, mqttPayloadReason(frame));

    mqttPayloadFree(&huge);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_fixedHeader_connect);
    RUN_TEST(test_fixedHeader_publish);
    RUN_TEST(test_fixedHeader_pingreq);
    RUN_TEST(test_fixedHeader_disconnect);

    RUN_TEST(test_connect_minimal);
    RUN_TEST(test_connect_with_credentials);
    RUN_TEST(test_connect_with_will);
    RUN_TEST(test_connect_protocol_name_recognised);
    RUN_TEST(test_connect_foreign_protocol_name_rejected);
    RUN_TEST(test_connect_shorter_protocol_name_does_not_desync);
    RUN_TEST(test_connect_truncated_protocol_name_is_invalid);
    RUN_TEST(test_connect_truncated_after_protocol_name_is_invalid);
    RUN_TEST(test_connect_keepalive);
    RUN_TEST(test_connect_protocol_version);

    RUN_TEST(test_publish_topic_and_payload);
    RUN_TEST(test_publish_message_id);
    RUN_TEST(test_publish_message_id_high);
    RUN_TEST(test_publish_without_a_message_id_is_qos0);
    RUN_TEST(test_publish_with_a_message_id_is_qos1);
    RUN_TEST(test_publish_retain_flag);

    RUN_TEST(test_puback_message_id);

    RUN_TEST(test_pubrec_type);
    RUN_TEST(test_pubrec_message_id);
    RUN_TEST(test_pubrec_message_id_high);
    RUN_TEST(test_pubrec_no_payload);

    RUN_TEST(test_pubrel_type);
    RUN_TEST(test_pubrel_qos_flag);
    RUN_TEST(test_pubrel_message_id);
    RUN_TEST(test_pubrel_message_id_high);
    RUN_TEST(test_pubrel_no_payload);

    RUN_TEST(test_pubcomp_type);
    RUN_TEST(test_pubcomp_message_id);
    RUN_TEST(test_pubcomp_message_id_high);
    RUN_TEST(test_pubcomp_no_payload);

    RUN_TEST(test_subscribe_single_topic);
    RUN_TEST(test_subscribe_multiple_topics);
    RUN_TEST(test_subscribe_message_id);

    RUN_TEST(test_unsubscribe_message_id);

    RUN_TEST(test_accessors_reject_wrong_type);
    RUN_TEST(test_accessors_clear_their_output_on_refusal);
    RUN_TEST(test_accessors_reject_null_arguments);
    RUN_TEST(test_accessors_reject_unusable_message);
    RUN_TEST(test_accessor_can_be_called_repeatedly);

    RUN_TEST(test_short_read_is_incomplete);
    RUN_TEST(test_self_contradicting_frame_is_malformed);

    RUN_TEST(test_publish_empty_topic_keeps_topic_field);
    RUN_TEST(test_publish_topic_is_a_view);
    RUN_TEST(test_connect_strings_are_views);
    RUN_TEST(test_subscribe_topics_are_views);
    RUN_TEST(test_cursor_can_be_walked_twice);

    RUN_TEST(test_well_formed_message_is_valid);
    RUN_TEST(test_truncated_publish_topic_is_invalid);
    RUN_TEST(test_truncated_connect_is_invalid);
    RUN_TEST(test_truncated_subscribe_is_invalid);

    RUN_TEST(test_publish_qos1_without_message_id_is_rejected);
    RUN_TEST(test_suback_without_message_id_is_rejected);
    RUN_TEST(test_puback_without_message_id_is_rejected);
    RUN_TEST(test_connack_without_return_code_is_rejected);

    RUN_TEST(test_suback_granted_qos);
    RUN_TEST(test_suback_granted_qos_is_a_view);

    RUN_TEST(test_unsubscribe_topics);

    RUN_TEST(test_makeString_length_prefix);
    RUN_TEST(test_makeStringPayload_no_prefix);

    RUN_TEST(test_frame_longer_than_the_protocol_allows_is_refused);

    return UNITY_END();
}
