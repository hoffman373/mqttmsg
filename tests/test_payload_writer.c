/* Tests for the internal frame writer (src/mqttmsg/payload_writer.h).

   The measuring pass and the overflow guard are the two properties the
   build path relies on and that nothing else would catch: if measuring
   silently disagreed with writing, every frame would still round-trip
   while the encoder wrote past its allocation. */

#include "unity/unity.h"
#include "mqttmsg/payload_writer.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ── Measuring pass ───────────────────────────────────────────────── */

void test_measure_writes_nothing(void) {
    PayloadWriter w = writerMeasure();
    writeByte(&w, 0xAA);
    putString(&w, "hello");
    TEST_ASSERT_NULL(w.buffer);
    TEST_ASSERT_TRUE(w.measuring);
    TEST_ASSERT_FALSE(w.overflowed);
}

void test_measure_counts_bytes(void) {
    PayloadWriter w = writerMeasure();
    writeByte(&w, 0x10);    /* 1 */
    writeShort(&w, 0x1234); /* 2 */
    putString(&w, "abc");   /* 2 + 3 */
    TEST_ASSERT_EQUAL_size_t(8, w.writePos);
}

void test_measure_never_overflows(void) {
    /* A measuring pass has no capacity to exceed. */
    PayloadWriter w = writerMeasure();
    for (int i = 0; i < 1000; i++) {
        putString(&w, "some/topic/name");
    }
    TEST_ASSERT_FALSE(w.overflowed);
    TEST_ASSERT_EQUAL_size_t(1000 * 17, w.writePos);
}

/* The property the whole design rests on: measuring and writing agree. */
void test_measure_matches_write(void) {
    uint8_t buffer[64];

    PayloadWriter measure = writerMeasure();
    writeByte(&measure, 0x30);
    putString(&measure, "home/temp");
    writeShort(&measure, 0x1234);
    writeBytes(&measure, (const uint8_t*)"body", 4);

    PayloadWriter w = writerOver(buffer, sizeof(buffer));
    writeByte(&w, 0x30);
    putString(&w, "home/temp");
    writeShort(&w, 0x1234);
    writeBytes(&w, (const uint8_t*)"body", 4);

    TEST_ASSERT_EQUAL_size_t(measure.writePos, w.writePos);
    TEST_ASSERT_FALSE(w.overflowed);
}

/* ── A failed allocation is not a measuring pass ──────────────────── */

/* The distinction the measuring flag exists for. Both writers have a NULL
   buffer, and before the flag they behaved identically: writerOver(NULL)
   accepted every write, discarded it, advanced writePos and left overflowed
   false — so mqttMakeString() returned a MqttPayload claiming bytes at address zero
   and nothing anywhere reported a failure. */
void test_writer_over_null_is_not_a_measuring_writer(void) {
    PayloadWriter failed = writerOver(NULL, 16);
    putString(&failed, "hello");

    TEST_ASSERT_TRUE(failed.overflowed);
    TEST_ASSERT_FALSE(failed.measuring);
    /* Nothing was written, so nothing may be claimed as written. */
    TEST_ASSERT_EQUAL_size_t(0, failed.writePos);

    PayloadWriter measuring = writerMeasure();
    putString(&measuring, "hello");

    TEST_ASSERT_FALSE(measuring.overflowed);
    TEST_ASSERT_TRUE(measuring.measuring);
    TEST_ASSERT_EQUAL_size_t(7, measuring.writePos);
}

void test_writer_over_null_stays_overflowed(void) {
    /* Sticky, like any other overflow: a caller checking once at the end
       still sees it. */
    PayloadWriter w = writerOver(NULL, 64);
    writeByte(&w, 0x01);
    writeShort(&w, 0x0203);
    writeBytes(&w, (const uint8_t*)"body", 4);

    TEST_ASSERT_TRUE(w.overflowed);
    TEST_ASSERT_EQUAL_size_t(0, w.writePos);
}

/* ── Encoding ─────────────────────────────────────────────────────── */

void test_putString_length_prefix(void) {
    uint8_t buffer[16] = {};
    PayloadWriter w = writerOver(buffer, sizeof(buffer));
    putString(&w, "hi");
    TEST_ASSERT_EQUAL_size_t(4, w.writePos);
    TEST_ASSERT_EQUAL_UINT8(0x00, buffer[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, buffer[1]);
    TEST_ASSERT_EQUAL_INT(0, memcmp(buffer + 2, "hi", 2));
}

void test_putView_handles_non_terminated(void) {
    const char raw[] = "abcdefgh";
    uint8_t buffer[16] = {};
    PayloadWriter w = writerOver(buffer, sizeof(buffer));
    putView(&w, mqttStrFromBytes(raw, 3));
    TEST_ASSERT_EQUAL_size_t(5, w.writePos);
    TEST_ASSERT_EQUAL_UINT8(0x03, buffer[1]);
    TEST_ASSERT_EQUAL_INT(0, memcmp(buffer + 2, "abc", 3));
}

void test_writeShort_is_big_endian(void) {
    uint8_t buffer[4] = {};
    PayloadWriter w = writerOver(buffer, sizeof(buffer));
    writeShort(&w, 0x1234);
    TEST_ASSERT_EQUAL_UINT8(0x12, buffer[0]);
    TEST_ASSERT_EQUAL_UINT8(0x34, buffer[1]);
}

void test_empty_string_is_just_a_prefix(void) {
    uint8_t buffer[8] = {};
    PayloadWriter w = writerOver(buffer, sizeof(buffer));
    putString(&w, "");
    TEST_ASSERT_EQUAL_size_t(2, w.writePos);
    TEST_ASSERT_EQUAL_UINT8(0x00, buffer[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buffer[1]);
}

/* ── Overflow guard ───────────────────────────────────────────────── */

void test_overflow_is_detected(void) {
    uint8_t buffer[4] = {};
    PayloadWriter w = writerOver(buffer, sizeof(buffer));
    putString(&w, "toolong"); /* needs 9, has 4 */
    TEST_ASSERT_TRUE(w.overflowed);
}

void test_overflow_writes_nothing_past_the_end(void) {
    /* The guard must refuse the write outright rather than truncate it —
       a partial write would corrupt whatever follows the buffer. */
    uint8_t buffer[8];
    memset(buffer, 0xEE, sizeof(buffer));

    PayloadWriter w = writerOver(buffer, 4);
    putString(&w, "toolong");

    TEST_ASSERT_TRUE(w.overflowed);
    for (size_t i = 0; i < sizeof(buffer); i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xEE, buffer[i],
                                        "writer touched memory it should not have");
    }
}

void test_overflow_is_sticky(void) {
    /* Once overflowed, later writes must stay no-ops even if they would
       have fit, so a caller checking the flag at the end still sees it. */
    uint8_t buffer[8];
    memset(buffer, 0xEE, sizeof(buffer));

    PayloadWriter w = writerOver(buffer, 8);
    putString(&w, "way too long for this");
    TEST_ASSERT_TRUE(w.overflowed);

    writeByte(&w, 0x01);
    TEST_ASSERT_TRUE(w.overflowed);
    TEST_ASSERT_EQUAL_UINT8(0xEE, buffer[0]);
}

void test_exact_fit_does_not_overflow(void) {
    uint8_t buffer[4] = {};
    PayloadWriter w = writerOver(buffer, sizeof(buffer));
    putString(&w, "hi"); /* exactly 4 */
    TEST_ASSERT_FALSE(w.overflowed);
    TEST_ASSERT_EQUAL_size_t(4, w.writePos);
}

void test_one_byte_over_overflows(void) {
    uint8_t buffer[4] = {};
    PayloadWriter w = writerOver(buffer, sizeof(buffer));
    putString(&w, "hi");
    writeByte(&w, 0x01);
    TEST_ASSERT_TRUE(w.overflowed);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_measure_writes_nothing);
    RUN_TEST(test_measure_counts_bytes);
    RUN_TEST(test_measure_never_overflows);
    RUN_TEST(test_measure_matches_write);

    RUN_TEST(test_writer_over_null_is_not_a_measuring_writer);
    RUN_TEST(test_writer_over_null_stays_overflowed);

    RUN_TEST(test_putString_length_prefix);
    RUN_TEST(test_putView_handles_non_terminated);
    RUN_TEST(test_writeShort_is_big_endian);
    RUN_TEST(test_empty_string_is_just_a_prefix);

    RUN_TEST(test_overflow_is_detected);
    RUN_TEST(test_overflow_writes_nothing_past_the_end);
    RUN_TEST(test_overflow_is_sticky);
    RUN_TEST(test_exact_fit_does_not_overflow);
    RUN_TEST(test_one_byte_over_overflows);

    return UNITY_END();
}
