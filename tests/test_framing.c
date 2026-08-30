/* Tests for the receive-side framing loop (src/mqttmsg/framing.c).

   This loop ran on the device for months with nothing over it. It is the
   code that decides where one message ends and the next begins, and every
   way it can be wrong is a way the parser gets handed bytes that are not a
   frame: a short read treated as complete, a coalesced pair treated as one
   message, a remainder shuffled to the wrong offset.

   None of that is visible from a single-frame parser test, because a
   single-frame test never exercises the boundaries. What follows drives
   real frames from the library's own builders through every arrival
   pattern TCP can produce — whole, coalesced, split at each byte, and
   malformed — and asserts that what comes out the other side is exactly
   the messages that went in.

   Run under -DMQTTMSG_SANITIZE=ON for the leak and bounds half of it. */

#include "unity/unity.h"
#include "framing.h"

#include <mqttmsg/mqtt.h>
#include <mqttmsg/mqtt_types.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_BUFFER_SIZE 2048
#define MAX_RECORDED 64

/* What the handler saw, so a test can assert on the sequence of whole
   messages rather than on buffer arithmetic. */
typedef struct {
    int count;
    MessageType types[MAX_RECORDED];
    uint32_t lengths[MAX_RECORDED];
    /* A copy of each frame's bytes: the views handed to the handler die
       when the loop moves on, so anything checked later must be copied. */
    uint8_t bytes[MAX_RECORDED][256];
    /* Set if a frame arrived that parseMessage could not make sense of.
       Nothing in these tests should produce one. */
    bool sawUnparsable;
} Recorder;

static Recorder recorder;
static uint8_t frameBuffer[FRAME_BUFFER_SIZE];

static void record(void* ctx, FixedHeader* fixedHeader, MqttPayload* payload) {
    Recorder* rec = (Recorder*)ctx;
    if (rec->count >= MAX_RECORDED) {
        TEST_FAIL_MESSAGE("recorder overflow — test fed more frames than it can hold");
        return;
    }

    /* Parse every frame the way the real handler does. parseMessage returns
       views into the payload and allocates nothing, so there is nothing to
       free here — but running it means ASan sees every read the parser
       makes against the exact bounds the framer handed over. That is the
       half of this that catches a framing bug as a memory error rather than
       as a wrong count. */
    Message parsed = parseMessage(*fixedHeader, *payload);
    if (parsed.status != MessageOk) {
        rec->sawUnparsable = true;
    }

    rec->types[rec->count] = fixedHeader->type;
    rec->lengths[rec->count] = payload->length;
    if (payload->length <= sizeof(rec->bytes[0])) {
        memcpy(rec->bytes[rec->count], payload->buffer, payload->length);
    }
    rec->count++;
}

/* buildPublish() copies the body into the frame it returns and leaves the
   caller holding the original — so every makeStringPayload() needs its own
   free(). Wrapping it here rather than repeating the pair 3000 times in the
   soak: the sanitized build caught exactly this omission the first time it
   ran, once per call site. */
static MqttPayload publishFrame(const char* topic, uint16_t messageId, const char* body,
                                bool retain) {
    MqttPayload inner = makeStringPayload(body);
    MqttPayload frame = buildPublish(topic, messageId, inner, retain);
    mqttPayloadFree(&inner);
    return frame;
}

static MessageFramer makeFramer(void) {
    MessageFramer framer = {
        .buffer = frameBuffer,
        .capacity = FRAME_BUFFER_SIZE,
        .length = 0,
        .onMessage = record,
        .ctx = &recorder,
    };
    return framer;
}

void setUp(void) {
    memset(&recorder, 0, sizeof(recorder));
    memset(frameBuffer, 0, sizeof(frameBuffer));
}

void tearDown(void) {}

/* Shorthands. This file is almost entirely "feed these bytes" and "compare
   against those bytes", and spelling the accessors out at every call buries
   what each assertion is about. */
static const uint8_t* frameBytes(MqttPayload p) { return mqttPayloadBytes(p); }

static int frameLen(MqttPayload p) { return (int)mqttPayloadLength(p); }

/* ── One frame at a time ──────────────────────────────────────────── */

void test_single_frame_is_dispatched_whole(void) {
    MessageFramer framer = makeFramer();
    MqttPayload frame = publishFrame("home/temp", 0, "21.5", false);

    TEST_ASSERT_EQUAL_INT(FramingOk, framerFeed(&framer, frameBytes(frame), frameLen(frame)));
    TEST_ASSERT_EQUAL_INT(1, recorder.count);
    TEST_ASSERT_EQUAL_INT(Publish, recorder.types[0]);
    TEST_ASSERT_EQUAL_UINT32(frameLen(frame), recorder.lengths[0]);
    TEST_ASSERT_EQUAL_MEMORY(frameBytes(frame), recorder.bytes[0], frameLen(frame));
    /* Consumed exactly: nothing left waiting. */
    TEST_ASSERT_EQUAL_INT(0, framer.length);
    TEST_ASSERT_FALSE(recorder.sawUnparsable);

    mqttPayloadFree(&frame);
}

/* ── Coalesced: several frames in one read ────────────────────────── */

void test_coalesced_frames_are_split(void) {
    MessageFramer framer = makeFramer();
    MqttPayload first = publishFrame("a/b", 0, "one", false);
    MqttPayload second = publishFrame("c/d/e", 0, "two", false);
    MqttPayload third = buildPingReq();

    uint8_t joined[512];
    int len = 0;
    memcpy(joined + len, frameBytes(first), frameLen(first));
    len += frameLen(first);
    memcpy(joined + len, frameBytes(second), frameLen(second));
    len += frameLen(second);
    memcpy(joined + len, frameBytes(third), frameLen(third));
    len += frameLen(third);

    TEST_ASSERT_EQUAL_INT(FramingOk, framerFeed(&framer, joined, len));

    TEST_ASSERT_EQUAL_INT(3, recorder.count);
    TEST_ASSERT_EQUAL_INT(Publish, recorder.types[0]);
    TEST_ASSERT_EQUAL_INT(Publish, recorder.types[1]);
    TEST_ASSERT_EQUAL_INT(PingReq, recorder.types[2]);
    TEST_ASSERT_EQUAL_UINT32(frameLen(first), recorder.lengths[0]);
    TEST_ASSERT_EQUAL_UINT32(frameLen(second), recorder.lengths[1]);
    /* The second frame must be its own bytes, not the tail of the first. */
    TEST_ASSERT_EQUAL_MEMORY(frameBytes(second), recorder.bytes[1], frameLen(second));
    TEST_ASSERT_EQUAL_INT(0, framer.length);
    TEST_ASSERT_FALSE(recorder.sawUnparsable);

    mqttPayloadFree(&first);
    mqttPayloadFree(&second);
    mqttPayloadFree(&third);
}

/* ── Split: one frame across several reads ────────────────────────── */

void test_partial_frame_waits_for_the_rest(void) {
    MessageFramer framer = makeFramer();
    MqttPayload frame = publishFrame("split/topic", 0, "payload", false);

    /* Everything but the last byte: a complete header, an incomplete body. */
    TEST_ASSERT_EQUAL_INT(FramingOk, framerFeed(&framer, frameBytes(frame), frameLen(frame) - 1));
    TEST_ASSERT_EQUAL_INT(0, recorder.count);
    TEST_ASSERT_EQUAL_INT(frameLen(frame) - 1, framer.length);

    TEST_ASSERT_EQUAL_INT(FramingOk,
                          framerFeed(&framer, frameBytes(frame) + frameLen(frame) - 1, 1));
    TEST_ASSERT_EQUAL_INT(1, recorder.count);
    TEST_ASSERT_EQUAL_MEMORY(frameBytes(frame), recorder.bytes[0], frameLen(frame));
    TEST_ASSERT_EQUAL_INT(0, framer.length);

    mqttPayloadFree(&frame);
}

void test_frame_split_at_every_byte_still_arrives_once(void) {
    /* The boundary cases that matter are the ones inside the fixed header,
       and they are cheap enough to test exhaustively rather than guess at. */
    MqttPayload frame = publishFrame("home/sensor/humidity", 0, "55", false);

    for (int cut = 1; cut < frameLen(frame); cut++) {
        memset(&recorder, 0, sizeof(recorder));
        MessageFramer framer = makeFramer();

        TEST_ASSERT_EQUAL_INT(FramingOk, framerFeed(&framer, frameBytes(frame), cut));
        TEST_ASSERT_EQUAL_INT(0, recorder.count);

        TEST_ASSERT_EQUAL_INT(FramingOk,
                              framerFeed(&framer, frameBytes(frame) + cut, frameLen(frame) - cut));

        char message[64];
        snprintf(message, sizeof(message), "cut at byte %d", cut);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, recorder.count, message);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(frameBytes(frame), recorder.bytes[0], frameLen(frame),
                                         message);
        TEST_ASSERT_EQUAL_INT(0, framer.length);
    }

    mqttPayloadFree(&frame);
}

void test_remainder_survives_across_feeds(void) {
    /* One whole frame plus the first bytes of a second, then the rest: the
       memmove has to land the partial frame at the front of the buffer, or
       the second message parses as garbage. */
    MessageFramer framer = makeFramer();
    /* Non-zero message ids, so these are QoS 1 frames carrying a packet
       identifier — the shape a session actually publishes, since
       mqttSessionNextMessageId() never hands out 0. */
    MqttPayload first = publishFrame("first/topic", 1, "1", false);
    MqttPayload second = publishFrame("second/topic", 2, "2", false);

    uint8_t chunk[512];
    int headOfSecond = 3;
    memcpy(chunk, frameBytes(first), frameLen(first));
    memcpy(chunk + frameLen(first), frameBytes(second), headOfSecond);

    TEST_ASSERT_EQUAL_INT(FramingOk, framerFeed(&framer, chunk, frameLen(first) + headOfSecond));
    TEST_ASSERT_EQUAL_INT(1, recorder.count);
    TEST_ASSERT_EQUAL_INT(headOfSecond, framer.length);

    TEST_ASSERT_EQUAL_INT(FramingOk, framerFeed(&framer, frameBytes(second) + headOfSecond,
                                                frameLen(second) - headOfSecond));
    TEST_ASSERT_EQUAL_INT(2, recorder.count);
    TEST_ASSERT_EQUAL_MEMORY(frameBytes(second), recorder.bytes[1], frameLen(second));
    TEST_ASSERT_FALSE(recorder.sawUnparsable);

    mqttPayloadFree(&first);
    mqttPayloadFree(&second);
}

/* ── Every message type through the loop ──────────────────────────── */

void test_every_built_type_round_trips_through_the_framer(void) {
    /* parseMessage has a branch per type, and each reads different fields
       at different offsets. A framing bug that only bites one of them —
       an off-by-one that happens to be harmless for PUBLISH — shows up
       here, or under ASan as a read past the frame. */
    Subscription subs[2] = {{.topic = "a/b", .qos = AtMostOnce},
                            {.topic = "c/#", .qos = AtLeastOnce}};
    const char* unsubTopics[2] = {"a/b", "c/#"};

    MqttPayload frames[] = {
        buildConnect("client-id", NULL, NULL, "will/topic", "offline", true, 90),
        publishFrame("some/topic", 7, "body", true),
        buildPublishAck(7),
        buildPubRec(8),
        buildPubRel(9),
        buildPubComp(10),
        buildSubscribe(11, subs, 2),
        buildUnsubscribe(12, unsubTopics, 2),
        buildPingReq(),
        buildDisconnectMsg(),
    };
    const int frameCount = (int)(sizeof(frames) / sizeof(frames[0]));

    MessageFramer framer = makeFramer();
    for (int i = 0; i < frameCount; i++) {
        TEST_ASSERT_EQUAL_INT(FramingOk,
                              framerFeed(&framer, frameBytes(frames[i]), frameLen(frames[i])));
    }

    TEST_ASSERT_EQUAL_INT(frameCount, recorder.count);
    for (int i = 0; i < frameCount; i++) {
        char message[64];
        snprintf(message, sizeof(message), "frame %d", i);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(frameLen(frames[i]), recorder.lengths[i], message);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(frameBytes(frames[i]), recorder.bytes[i],
                                         frameLen(frames[i]), message);
        mqttPayloadFree(&frames[i]);
    }
}

/* ── The soak ─────────────────────────────────────────────────────── */

void test_soak_thousands_of_frames_in_varied_arrival_patterns(void) {
    /* The point of volume here is not statistics, it is state: the framer
       carries a buffer across every one of these, and a leak, an overrun or
       a bookkeeping slip compounds until it is unmissable — the class of bug
       that on a device produces no symptom until the watchdog fires hours
       later.

       Topic lengths vary so the remaining-length field crosses the
       one-to-two byte boundary, and the chunking rotates through whole,
       byte-at-a-time and split-in-half so no single arrival pattern
       dominates. */
    MessageFramer framer = makeFramer();
    const int iterations = 3000;
    int dispatched = 0;

    for (int i = 0; i < iterations; i++) {
        char topic[160];
        int topicLen = 3 + (i % 120);
        memset(topic, 'x', (size_t)topicLen);
        topic[0] = 't';
        topic[topicLen] = '\0';

        char body[64];
        snprintf(body, sizeof(body), "reading-%d", i);

        bool qos1 = (i % 3) == 0;
        MqttPayload frame = publishFrame(topic, qos1 ? (uint16_t)(i + 1) : 0, body, (i % 5) == 0);

        recorder.count = 0;
        switch (i % 3) {
            case 0:
                TEST_ASSERT_EQUAL_INT(FramingOk,
                                      framerFeed(&framer, frameBytes(frame), frameLen(frame)));
                break;
            case 1:
                for (int b = 0; b < frameLen(frame); b++) {
                    TEST_ASSERT_EQUAL_INT(FramingOk, framerFeed(&framer, frameBytes(frame) + b, 1));
                }
                break;
            default: {
                int half = frameLen(frame) / 2;
                TEST_ASSERT_EQUAL_INT(FramingOk, framerFeed(&framer, frameBytes(frame), half));
                TEST_ASSERT_EQUAL_INT(FramingOk, framerFeed(&framer, frameBytes(frame) + half,
                                                            frameLen(frame) - half));
                break;
            }
        }

        TEST_ASSERT_EQUAL_INT(1, recorder.count);
        TEST_ASSERT_EQUAL_UINT32(frameLen(frame), recorder.lengths[0]);
        dispatched += recorder.count;

        mqttPayloadFree(&frame);
    }

    TEST_ASSERT_EQUAL_INT(iterations, dispatched);
    /* Every frame accounted for means the buffer came back to empty. */
    TEST_ASSERT_EQUAL_INT(0, framer.length);
    TEST_ASSERT_FALSE(recorder.sawUnparsable);
}

/* ── Malformed input ──────────────────────────────────────────────── */

void test_truncated_fixed_header_is_not_a_message(void) {
    MessageFramer framer = makeFramer();
    /* A type byte and nothing else: the remaining-length field has not
       arrived, so there is no header yet — incomplete, not invalid. */
    uint8_t typeByteOnly[] = {0x30};

    TEST_ASSERT_EQUAL_INT(FramingOk, framerFeed(&framer, typeByteOnly, 1));
    TEST_ASSERT_EQUAL_INT(0, recorder.count);
    TEST_ASSERT_EQUAL_INT(1, framer.length);
}

void test_remaining_length_over_four_bytes_is_malformed(void) {
    MessageFramer framer = makeFramer();
    /* Five continuation bytes: the protocol allows at most four, so this
       stream can never resynchronise by waiting for more. */
    uint8_t runaway[] = {0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};

    TEST_ASSERT_EQUAL_INT(FramingMalformed, framerFeed(&framer, runaway, (int)sizeof(runaway)));
    TEST_ASSERT_EQUAL_INT(0, recorder.count);
    /* Emptied, so the caller's reconnect starts from a clean buffer. */
    TEST_ASSERT_EQUAL_INT(0, framer.length);
}

void test_length_pointing_past_the_buffer_waits_rather_than_reads(void) {
    /* A header claiming 200 bytes with 4 delivered. The framer must hold,
       not hand the parser a payload view running off the end of what
       arrived — under ASan, getting this wrong is a heap overflow. */
    MessageFramer framer = makeFramer();
    uint8_t overlong[] = {0x30, 200, 'a', 'b'};

    TEST_ASSERT_EQUAL_INT(FramingOk, framerFeed(&framer, overlong, (int)sizeof(overlong)));
    TEST_ASSERT_EQUAL_INT(0, recorder.count);
    TEST_ASSERT_EQUAL_INT((int)sizeof(overlong), framer.length);
}

void test_oversized_input_is_refused_without_copying(void) {
    MessageFramer framer = makeFramer();
    static uint8_t huge[FRAME_BUFFER_SIZE + 1];
    memset(huge, 0x30, sizeof(huge));

    TEST_ASSERT_EQUAL_INT(FramingOverflow, framerFeed(&framer, huge, (int)sizeof(huge)));
    TEST_ASSERT_EQUAL_INT(0, recorder.count);
    TEST_ASSERT_EQUAL_INT(0, framer.length);
}

void test_a_good_frame_after_a_malformed_one_is_read_from_a_clean_buffer(void) {
    MessageFramer framer = makeFramer();
    uint8_t runaway[] = {0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL_INT(FramingMalformed, framerFeed(&framer, runaway, (int)sizeof(runaway)));

    /* The real caller drops the connection here; this asserts the framer is
       left in a state where it would not matter if it did not. */
    MqttPayload frame = publishFrame("after/reset", 0, "ok", false);
    TEST_ASSERT_EQUAL_INT(FramingOk, framerFeed(&framer, frameBytes(frame), frameLen(frame)));
    TEST_ASSERT_EQUAL_INT(1, recorder.count);
    TEST_ASSERT_EQUAL_MEMORY(frameBytes(frame), recorder.bytes[0], frameLen(frame));

    mqttPayloadFree(&frame);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_single_frame_is_dispatched_whole);

    RUN_TEST(test_coalesced_frames_are_split);

    RUN_TEST(test_partial_frame_waits_for_the_rest);
    RUN_TEST(test_frame_split_at_every_byte_still_arrives_once);
    RUN_TEST(test_remainder_survives_across_feeds);

    RUN_TEST(test_every_built_type_round_trips_through_the_framer);
    RUN_TEST(test_soak_thousands_of_frames_in_varied_arrival_patterns);

    RUN_TEST(test_truncated_fixed_header_is_not_a_message);
    RUN_TEST(test_remaining_length_over_four_bytes_is_malformed);
    RUN_TEST(test_length_pointing_past_the_buffer_waits_rather_than_reads);
    RUN_TEST(test_oversized_input_is_refused_without_copying);
    RUN_TEST(test_a_good_frame_after_a_malformed_one_is_read_from_a_clean_buffer);

    return UNITY_END();
}
