/* Tests for include/mqttmsg/version.h.

   Almost all of this header is preprocessor, which fails quietly: a
   mis-stringified macro yields "MQTTMSG_VERSION_MAJOR.MQTTMSG_VERSION_MINOR"
   rather than an error, and a version comparison that does not order
   correctly still compiles. Both are checked here.

   CMakeLists.txt parses the same three numbers back out of this header to
   set project(VERSION), so a change that breaks the format fails at
   configure time rather than here. */

#include "unity/unity.h"
#include <mqttmsg/version.h>
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ── The combined integer ─────────────────────────────────────────── */

void test_version_matches_its_parts(void) {
    TEST_ASSERT_EQUAL_INT(
        MQTTMSG_VERSION_ENCODE(MQTTMSG_VERSION_MAJOR, MQTTMSG_VERSION_MINOR, MQTTMSG_VERSION_PATCH),
        MQTTMSG_VERSION);
}

void test_encoding_orders_by_significance(void) {
    /* The point of the macro: a consumer writes #if MQTTMSG_VERSION >= X and
       gets an answer that respects all three numbers. */
    TEST_ASSERT_TRUE(MQTTMSG_VERSION_ENCODE(0, 1, 1) > MQTTMSG_VERSION_ENCODE(0, 1, 0));
    TEST_ASSERT_TRUE(MQTTMSG_VERSION_ENCODE(0, 2, 0) > MQTTMSG_VERSION_ENCODE(0, 1, 99));
    TEST_ASSERT_TRUE(MQTTMSG_VERSION_ENCODE(1, 0, 0) > MQTTMSG_VERSION_ENCODE(0, 99, 99));
}

void test_encoding_holds_to_the_documented_ceiling(void) {
    /* Two decimal digits each for minor and patch. At 100 the ordering
       collapses, which is what the header says and what would silently
       break a consumer's feature test. */
    TEST_ASSERT_EQUAL_INT(MQTTMSG_VERSION_ENCODE(1, 0, 0), MQTTMSG_VERSION_ENCODE(0, 100, 0));
}

/* ── The string ───────────────────────────────────────────────────── */

void test_version_string_is_the_numbers(void) {
    /* Guards the stringify indirection: one level too few expands to the
       macro names instead of their values, and still compiles. */
    char expected[32];
    snprintf(expected, sizeof(expected), "%d.%d.%d", MQTTMSG_VERSION_MAJOR, MQTTMSG_VERSION_MINOR,
             MQTTMSG_VERSION_PATCH);

    TEST_ASSERT_EQUAL_STRING(expected, MQTTMSG_VERSION_STRING);
}

void test_version_string_carries_no_macro_names(void) {
    TEST_ASSERT_NULL(strstr(MQTTMSG_VERSION_STRING, "MQTTMSG"));
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_version_matches_its_parts);
    RUN_TEST(test_encoding_orders_by_significance);
    RUN_TEST(test_encoding_holds_to_the_documented_ceiling);

    RUN_TEST(test_version_string_is_the_numbers);
    RUN_TEST(test_version_string_carries_no_macro_names);

    return UNITY_END();
}
