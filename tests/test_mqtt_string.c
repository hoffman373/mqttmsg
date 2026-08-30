#include "unity/unity.h"
#include <mqttmsg/mqtt_string.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ── Construction ─────────────────────────────────────────────────── */

void test_empty_is_empty(void) {
    MqttString s = mqttStrEmpty();
    TEST_ASSERT_TRUE(mqttStrIsEmpty(s));
    TEST_ASSERT_EQUAL_UINT16(0, s.length);
}

/* Present and empty are different questions, and the wire encoding depends
   on which one is being asked: an absent field contributes no bytes, a
   present empty one contributes a 0x0000 length prefix. */
void test_absent_is_not_present(void) {
    TEST_ASSERT_FALSE(mqttStrIsPresent(mqttStrEmpty()));
    TEST_ASSERT_TRUE(mqttStrIsEmpty(mqttStrEmpty()));
}

void test_empty_string_is_present(void) {
    MqttString s = mqttStrFromCStr("");
    TEST_ASSERT_TRUE(mqttStrIsPresent(s));
    TEST_ASSERT_TRUE(mqttStrIsEmpty(s));
}

void test_nonempty_is_present(void) {
    MqttString s = mqttStrFromCStr("a/b");
    TEST_ASSERT_TRUE(mqttStrIsPresent(s));
    TEST_ASSERT_FALSE(mqttStrIsEmpty(s));
}

void test_fromCStr_null_is_absent(void) {
    TEST_ASSERT_FALSE(mqttStrIsPresent(mqttStrFromCStr(NULL)));
}

void test_zero_length_view_over_a_buffer_is_present(void) {
    const char buffer[] = "abc";
    TEST_ASSERT_TRUE(mqttStrIsPresent(mqttStrFromBytes(buffer, 0)));
}

void test_fromCStr_length(void) {
    MqttString s = mqttStrFromCStr("home/temp");
    TEST_ASSERT_EQUAL_UINT16(9, s.length);
    TEST_ASSERT_FALSE(mqttStrIsEmpty(s));
}

void test_fromCStr_does_not_copy(void) {
    const char* original = "home/temp";
    MqttString s = mqttStrFromCStr(original);
    /* The whole point: the view aliases the caller's bytes. */
    TEST_ASSERT_EQUAL_PTR(original, s.data);
}

void test_fromCStr_null_is_empty(void) {
    MqttString s = mqttStrFromCStr(NULL);
    TEST_ASSERT_TRUE(mqttStrIsEmpty(s));
}

void test_fromCStr_empty_string(void) {
    MqttString s = mqttStrFromCStr("");
    TEST_ASSERT_TRUE(mqttStrIsEmpty(s));
}

void test_fromBytes_is_not_nul_terminated(void) {
    /* A view into the middle of a larger buffer, exactly as the parser
       produces: no terminator, and the following bytes are unrelated. */
    const char buffer[] = "home/tempSENSOR=42";
    MqttString s = mqttStrFromBytes(buffer, 9);
    TEST_ASSERT_EQUAL_UINT16(9, s.length);
    TEST_ASSERT_TRUE(mqttStrEqCStr(s, "home/temp"));
    TEST_ASSERT_EQUAL_CHAR('S', s.data[s.length]);
}

/* ── Equality ─────────────────────────────────────────────────────── */

void test_eq_same_content(void) {
    TEST_ASSERT_TRUE(mqttStrEq(mqttStrFromCStr("a/b"), mqttStrFromCStr("a/b")));
}

void test_eq_different_content(void) {
    TEST_ASSERT_FALSE(mqttStrEq(mqttStrFromCStr("a/b"), mqttStrFromCStr("a/c")));
}

void test_eq_different_length(void) {
    TEST_ASSERT_FALSE(mqttStrEq(mqttStrFromCStr("a/b"), mqttStrFromCStr("a/bc")));
}

void test_eq_respects_length_not_terminator(void) {
    /* "a/b" viewed out of a longer buffer must not compare equal to "a/bc". */
    const char buffer[] = "a/bc";
    MqttString s = mqttStrFromBytes(buffer, 3);
    TEST_ASSERT_TRUE(mqttStrEqCStr(s, "a/b"));
    TEST_ASSERT_FALSE(mqttStrEqCStr(s, "a/bc"));
}

void test_eq_empty_strings(void) {
    TEST_ASSERT_TRUE(mqttStrEq(mqttStrEmpty(), mqttStrFromCStr("")));
}

/* ── Prefix matching (the wildcard path) ──────────────────────────── */

void test_startsWith_true(void) {
    MqttString topic = mqttStrFromCStr("atx/1/command");
    TEST_ASSERT_TRUE(mqttStrStartsWithCStr(topic, "atx/"));
}

void test_startsWith_false(void) {
    MqttString topic = mqttStrFromCStr("atx/1/command");
    TEST_ASSERT_FALSE(mqttStrStartsWithCStr(topic, "abc/"));
}

void test_startsWith_longer_prefix_than_string(void) {
    MqttString topic = mqttStrFromCStr("atx");
    TEST_ASSERT_FALSE(mqttStrStartsWithCStr(topic, "atx/1"));
}

void test_startsWith_whole_string(void) {
    MqttString topic = mqttStrFromCStr("atx/1");
    TEST_ASSERT_TRUE(mqttStrStartsWithCStr(topic, "atx/1"));
}

void test_startsWith_empty_prefix(void) {
    TEST_ASSERT_TRUE(mqttStrStartsWithCStr(mqttStrFromCStr("atx"), ""));
}

/* This is the distinction that prefix-vs-exact matching turns on: the old
   dispatch() used strncmp with the subscription length, so "atx/1" matched
   the topic "atx/10". Exact match must not. */
void test_prefix_matches_where_equality_does_not(void) {
    MqttString topic = mqttStrFromCStr("atx/10");
    TEST_ASSERT_TRUE(mqttStrStartsWithCStr(topic, "atx/1"));
    TEST_ASSERT_FALSE(mqttStrEqCStr(topic, "atx/1"));
}

/* ── Copy out ─────────────────────────────────────────────────────── */

void test_toCStr_exact_fit(void) {
    char dst[16] = {};
    MqttString s = mqttStrFromCStr("home/temp");
    uint16_t wanted = mqttStrToCStr(dst, sizeof(dst), s);
    TEST_ASSERT_EQUAL_UINT16(9, wanted);
    TEST_ASSERT_EQUAL_STRING("home/temp", dst);
}

void test_toCStr_terminates_a_view(void) {
    const char buffer[] = "home/tempGARBAGE";
    char dst[16] = {};
    mqttStrToCStr(dst, sizeof(dst), mqttStrFromBytes(buffer, 9));
    TEST_ASSERT_EQUAL_STRING("home/temp", dst);
}

void test_toCStr_truncates_and_reports(void) {
    char dst[5] = {};
    MqttString s = mqttStrFromCStr("home/temp");
    uint16_t wanted = mqttStrToCStr(dst, sizeof(dst), s);
    /* snprintf semantics: returns what it wanted, so truncation is
       detectable by wanted >= dstSize. */
    TEST_ASSERT_EQUAL_UINT16(9, wanted);
    TEST_ASSERT_TRUE(wanted >= sizeof(dst));
    TEST_ASSERT_EQUAL_STRING("home", dst);
}

void test_toCStr_always_terminates(void) {
    char dst[4];
    memset(dst, 'X', sizeof(dst));
    mqttStrToCStr(dst, sizeof(dst), mqttStrFromCStr("abcdefg"));
    TEST_ASSERT_EQUAL_CHAR('\0', dst[3]);
}

void test_toCStr_zero_size_writes_nothing(void) {
    char dst[4];
    memset(dst, 'X', sizeof(dst));
    uint16_t wanted = mqttStrToCStr(dst, 0, mqttStrFromCStr("abc"));
    TEST_ASSERT_EQUAL_UINT16(3, wanted);
    TEST_ASSERT_EQUAL_CHAR('X', dst[0]);
}

void test_toCStr_null_dst_is_safe(void) {
    TEST_ASSERT_EQUAL_UINT16(3, mqttStrToCStr(NULL, 16, mqttStrFromCStr("abc")));
}

void test_toCStr_empty_source(void) {
    char dst[4];
    memset(dst, 'X', sizeof(dst));
    mqttStrToCStr(dst, sizeof(dst), mqttStrEmpty());
    TEST_ASSERT_EQUAL_STRING("", dst);
}

/* ── Segment cursor ───────────────────────────────────────────────── */

static int collectSegments(const char* topic, char out[][32], int maxOut) {
    MqttStrCursor cursor = mqttStrSegments(mqttStrFromCStr(topic));
    MqttString segment;
    int count = 0;
    while (count < maxOut && mqttStrNextSegment(&cursor, &segment)) {
        mqttStrToCStr(out[count], 32, segment);
        count++;
    }
    return count;
}

void test_segments_simple(void) {
    char got[8][32] = {};
    int n = collectSegments("atx/1/command", got, 8);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("atx", got[0]);
    TEST_ASSERT_EQUAL_STRING("1", got[1]);
    TEST_ASSERT_EQUAL_STRING("command", got[2]);
}

void test_segments_single(void) {
    char got[8][32] = {};
    int n = collectSegments("atx", got, 8);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("atx", got[0]);
}

void test_segments_empty_topic_yields_none(void) {
    char got[8][32] = {};
    TEST_ASSERT_EQUAL_INT(0, collectSegments("", got, 8));
}

void test_segments_empty_middle(void) {
    char got[8][32] = {};
    int n = collectSegments("a//b", got, 8);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("a", got[0]);
    TEST_ASSERT_EQUAL_STRING("", got[1]);
    TEST_ASSERT_EQUAL_STRING("b", got[2]);
}

void test_segments_trailing_separator(void) {
    char got[8][32] = {};
    int n = collectSegments("a/", got, 8);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("a", got[0]);
    TEST_ASSERT_EQUAL_STRING("", got[1]);
}

void test_segments_leading_separator(void) {
    char got[8][32] = {};
    int n = collectSegments("/a", got, 8);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("", got[0]);
    TEST_ASSERT_EQUAL_STRING("a", got[1]);
}

void test_segments_do_not_copy(void) {
    MqttString topic = mqttStrFromCStr("atx/1/command");
    MqttStrCursor cursor = mqttStrSegments(topic);
    MqttString segment;
    TEST_ASSERT_TRUE(mqttStrNextSegment(&cursor, &segment));
    /* Each segment aliases the original buffer rather than copying. */
    TEST_ASSERT_EQUAL_PTR(topic.data, segment.data);
}

void test_segments_respect_view_bounds(void) {
    /* Cursor must stop at the view's length, not run to a NUL. */
    const char buffer[] = "a/b/c";
    char got[8][32] = {};
    MqttStrCursor cursor = mqttStrSegments(mqttStrFromBytes(buffer, 3));
    MqttString segment;
    int n = 0;
    while (n < 8 && mqttStrNextSegment(&cursor, &segment)) {
        mqttStrToCStr(got[n], 32, segment);
        n++;
    }
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("a", got[0]);
    TEST_ASSERT_EQUAL_STRING("b", got[1]);
}

void test_segments_null_args_are_safe(void) {
    MqttStrCursor cursor = mqttStrSegments(mqttStrFromCStr("a/b"));
    MqttString segment;
    TEST_ASSERT_FALSE(mqttStrNextSegment(NULL, &segment));
    TEST_ASSERT_FALSE(mqttStrNextSegment(&cursor, NULL));
}

void test_segments_exhausted_stays_exhausted(void) {
    MqttStrCursor cursor = mqttStrSegments(mqttStrFromCStr("a"));
    MqttString segment;
    TEST_ASSERT_TRUE(mqttStrNextSegment(&cursor, &segment));
    TEST_ASSERT_FALSE(mqttStrNextSegment(&cursor, &segment));
    TEST_ASSERT_FALSE(mqttStrNextSegment(&cursor, &segment));
}

/* ── Topic prefix stripping ───────────────────────────────────────── */

void test_strip_prefix_removes_prefix_and_separator(void) {
    MqttString rest;
    TEST_ASSERT_TRUE(mqttStrStripTopicPrefixCStr(mqttStrFromCStr("atx/2/command"), "atx", &rest));
    TEST_ASSERT_TRUE(mqttStrEqCStr(rest, "2/command"));
}

/* The trap a bare mqttStrStartsWithCStr() falls into: without the
   separator, "atx" also matches a sibling topic that merely starts the
   same way. */
void test_strip_prefix_matches_whole_segments(void) {
    TEST_ASSERT_FALSE(
        mqttStrStripTopicPrefixCStr(mqttStrFromCStr("atxfoo/2/command"), "atx", NULL));
    TEST_ASSERT_TRUE(mqttStrStartsWithCStr(mqttStrFromCStr("atxfoo/2/command"), "atx"));
}

void test_strip_prefix_of_the_whole_topic_leaves_nothing(void) {
    MqttString rest;
    TEST_ASSERT_TRUE(mqttStrStripTopicPrefixCStr(mqttStrFromCStr("atx"), "atx", &rest));
    TEST_ASSERT_TRUE(mqttStrIsEmpty(rest));
    /* Present but empty: the topic was under the prefix, it just had nothing
       after it. */
    TEST_ASSERT_TRUE(mqttStrIsPresent(rest));
}

void test_strip_prefix_trailing_separator_leaves_nothing(void) {
    MqttString rest;
    TEST_ASSERT_TRUE(mqttStrStripTopicPrefixCStr(mqttStrFromCStr("atx/"), "atx", &rest));
    TEST_ASSERT_TRUE(mqttStrIsEmpty(rest));
}

void test_strip_prefix_rejects_a_different_topic(void) {
    MqttString rest = mqttStrFromCStr("untouched");
    TEST_ASSERT_FALSE(mqttStrStripTopicPrefixCStr(mqttStrFromCStr("other/2"), "atx", &rest));
    TEST_ASSERT_TRUE(mqttStrEqCStr(rest, "untouched"));
}

void test_strip_prefix_rejects_a_prefix_longer_than_the_topic(void) {
    TEST_ASSERT_FALSE(mqttStrStripTopicPrefixCStr(mqttStrFromCStr("at"), "atx", NULL));
}

void test_strip_prefix_empty_prefix_strips_nothing(void) {
    MqttString rest;
    TEST_ASSERT_TRUE(mqttStrStripTopicPrefixCStr(mqttStrFromCStr("atx/2"), "", &rest));
    TEST_ASSERT_TRUE(mqttStrEqCStr(rest, "atx/2"));
    TEST_ASSERT_TRUE(mqttStrStripTopicPrefixCStr(mqttStrFromCStr("atx/2"), NULL, &rest));
    TEST_ASSERT_TRUE(mqttStrEqCStr(rest, "atx/2"));
}

void test_strip_prefix_null_rest_is_a_predicate(void) {
    TEST_ASSERT_TRUE(mqttStrStripTopicPrefixCStr(mqttStrFromCStr("atx/2"), "atx", NULL));
}

void test_strip_prefix_does_not_copy(void) {
    const char* topic = "atx/2/command";
    MqttString rest;
    TEST_ASSERT_TRUE(mqttStrStripTopicPrefixCStr(mqttStrFromCStr(topic), "atx", &rest));
    TEST_ASSERT_EQUAL_PTR(topic + 4, rest.data);
}

/* ── Topic matching ───────────────────────────────────────────────── */

void test_match_literal_pattern(void) {
    TEST_ASSERT_TRUE(
        mqttTopicMatch(mqttStrFromCStr("atx/2/command"), "atx/2/command", NULL, 0, NULL));
    TEST_ASSERT_FALSE(
        mqttTopicMatch(mqttStrFromCStr("atx/2/state"), "atx/2/command", NULL, 0, NULL));
}

void test_match_captures_a_plus(void) {
    MqttString captures[1];
    uint8_t count = 0;
    TEST_ASSERT_TRUE(
        mqttTopicMatch(mqttStrFromCStr("atx/2/command"), "atx/+/command", captures, 1, &count));
    TEST_ASSERT_EQUAL_UINT8(1, count);
    TEST_ASSERT_TRUE(mqttStrEqCStr(captures[0], "2"));
}

void test_match_captures_in_pattern_order(void) {
    MqttString captures[2];
    uint8_t count = 0;
    TEST_ASSERT_TRUE(
        mqttTopicMatch(mqttStrFromCStr("atx/2/relay/on"), "atx/+/relay/+", captures, 2, &count));
    TEST_ASSERT_EQUAL_UINT8(2, count);
    TEST_ASSERT_TRUE(mqttStrEqCStr(captures[0], "2"));
    TEST_ASSERT_TRUE(mqttStrEqCStr(captures[1], "on"));
}

/* Anchored at the far end: a pattern that accounts for two segments does
   not match a three-segment topic, which is what makes this usable for
   routing rather than only for filtering. */
void test_match_rejects_an_unconsumed_trailing_segment(void) {
    TEST_ASSERT_FALSE(
        mqttTopicMatch(mqttStrFromCStr("atx/2/command/now"), "atx/+/command", NULL, 0, NULL));
}

void test_match_rejects_a_topic_that_runs_out(void) {
    TEST_ASSERT_FALSE(mqttTopicMatch(mqttStrFromCStr("atx/2"), "atx/+/command", NULL, 0, NULL));
}

void test_match_plus_captures_an_empty_segment(void) {
    MqttString captures[1];
    uint8_t count = 0;
    TEST_ASSERT_TRUE(
        mqttTopicMatch(mqttStrFromCStr("atx//command"), "atx/+/command", captures, 1, &count));
    TEST_ASSERT_EQUAL_UINT8(1, count);
    TEST_ASSERT_TRUE(mqttStrIsEmpty(captures[0]));
}

void test_match_hash_captures_the_rest(void) {
    MqttString captures[1];
    uint8_t count = 0;
    TEST_ASSERT_TRUE(
        mqttTopicMatch(mqttStrFromCStr("atx/2/relay/on"), "atx/#", captures, 1, &count));
    TEST_ASSERT_EQUAL_UINT8(1, count);
    TEST_ASSERT_TRUE(mqttStrEqCStr(captures[0], "2/relay/on"));
}

void test_match_hash_captures_one_segment(void) {
    MqttString captures[1];
    TEST_ASSERT_TRUE(mqttTopicMatch(mqttStrFromCStr("atx/2"), "atx/#", captures, 1, NULL));
    TEST_ASSERT_TRUE(mqttStrEqCStr(captures[0], "2"));
}

/* '#' stands for the remaining segments, and there being none of them is a
   case of that. */
void test_match_hash_captures_nothing_at_the_end(void) {
    MqttString captures[1];
    uint8_t count = 0;
    TEST_ASSERT_TRUE(mqttTopicMatch(mqttStrFromCStr("atx"), "atx/#", captures, 1, &count));
    TEST_ASSERT_EQUAL_UINT8(1, count);
    TEST_ASSERT_TRUE(mqttStrIsEmpty(captures[0]));
}

void test_match_hash_alone_takes_the_whole_topic(void) {
    MqttString captures[1];
    TEST_ASSERT_TRUE(mqttTopicMatch(mqttStrFromCStr("atx/2/command"), "#", captures, 1, NULL));
    TEST_ASSERT_TRUE(mqttStrEqCStr(captures[0], "atx/2/command"));
}

void test_match_rejects_a_hash_that_is_not_last(void) {
    TEST_ASSERT_FALSE(
        mqttTopicMatch(mqttStrFromCStr("atx/2/command"), "atx/#/command", NULL, 0, NULL));
}

/* More wildcards than the caller has room for still match; the ones past
   the end are simply not recorded, and the count says so. */
void test_match_never_writes_past_maxCaptures(void) {
    MqttString captures[3];
    uint8_t count = 0;
    captures[2] = mqttStrFromCStr("untouched");
    TEST_ASSERT_TRUE(mqttTopicMatch(mqttStrFromCStr("a/b/c"), "+/+/+", captures, 2, &count));
    TEST_ASSERT_EQUAL_UINT8(2, count);
    TEST_ASSERT_TRUE(mqttStrEqCStr(captures[0], "a"));
    TEST_ASSERT_TRUE(mqttStrEqCStr(captures[1], "b"));
    TEST_ASSERT_TRUE(mqttStrEqCStr(captures[2], "untouched"));
}

void test_match_without_capturing(void) {
    uint8_t count = 9;
    TEST_ASSERT_TRUE(mqttTopicMatch(mqttStrFromCStr("a/b"), "+/+", NULL, 4, &count));
    TEST_ASSERT_EQUAL_UINT8(0, count);
}

void test_match_zeroes_the_count_on_a_failed_match(void) {
    MqttString captures[2];
    uint8_t count = 9;
    TEST_ASSERT_FALSE(mqttTopicMatch(mqttStrFromCStr("a/b/c"), "+/+", captures, 2, &count));
    TEST_ASSERT_EQUAL_UINT8(0, count);
}

void test_match_empty_pattern_matches_only_an_empty_topic(void) {
    TEST_ASSERT_TRUE(mqttTopicMatch(mqttStrFromCStr(""), "", NULL, 0, NULL));
    TEST_ASSERT_FALSE(mqttTopicMatch(mqttStrFromCStr("a"), "", NULL, 0, NULL));
}

void test_match_respects_view_bounds(void) {
    const char buffer[] = "a/b/c";
    TEST_ASSERT_TRUE(mqttTopicMatch(mqttStrFromBytes(buffer, 3), "a/b", NULL, 0, NULL));
    TEST_ASSERT_FALSE(mqttTopicMatch(mqttStrFromBytes(buffer, 3), "a/b/c", NULL, 0, NULL));
}

void test_match_captures_borrow_the_topic(void) {
    const char* topic = "atx/2/command";
    MqttString captures[1];
    TEST_ASSERT_TRUE(mqttTopicMatch(mqttStrFromCStr(topic), "atx/+/command", captures, 1, NULL));
    TEST_ASSERT_EQUAL_PTR(topic + 4, captures[0].data);
}

/* ── Topic matching under a prefix ────────────────────────────────── */

void test_matchUnder_strips_then_matches(void) {
    MqttString captures[1];
    uint8_t count = 0;
    TEST_ASSERT_TRUE(mqttTopicMatchUnder(mqttStrFromCStr("atx/2/command"), "atx", "+/command",
                                         captures, 1, &count));
    TEST_ASSERT_EQUAL_UINT8(1, count);
    TEST_ASSERT_TRUE(mqttStrEqCStr(captures[0], "2"));
}

void test_matchUnder_rejects_a_partial_prefix_segment(void) {
    uint8_t count = 9;
    TEST_ASSERT_FALSE(mqttTopicMatchUnder(mqttStrFromCStr("atxfoo/2/command"), "atx", "+/command",
                                          NULL, 0, &count));
    TEST_ASSERT_EQUAL_UINT8(0, count);
}

void test_matchUnder_rejects_the_rest_not_matching(void) {
    TEST_ASSERT_FALSE(
        mqttTopicMatchUnder(mqttStrFromCStr("atx/2/state"), "atx", "+/command", NULL, 0, NULL));
    TEST_ASSERT_FALSE(mqttTopicMatchUnder(mqttStrFromCStr("atx/2/command/now"), "atx", "+/command",
                                          NULL, 0, NULL));
    TEST_ASSERT_FALSE(
        mqttTopicMatchUnder(mqttStrFromCStr("atx/2"), "atx", "+/command", NULL, 0, NULL));
}

void test_matchUnder_no_prefix_is_a_plain_match(void) {
    TEST_ASSERT_TRUE(mqttTopicMatchUnder(mqttStrFromCStr("atx/2"), NULL, "atx/+", NULL, 0, NULL));
}

/* ── Parsing an unsigned integer ──────────────────────────────────── */

void test_toU32_parses_digits(void) {
    uint32_t value = 0;
    TEST_ASSERT_TRUE(mqttStrToU32(mqttStrFromCStr("2"), &value));
    TEST_ASSERT_EQUAL_UINT32(2, value);
    TEST_ASSERT_TRUE(mqttStrToU32(mqttStrFromCStr("1234567890"), &value));
    TEST_ASSERT_EQUAL_UINT32(1234567890u, value);
}

void test_toU32_accepts_leading_zeros(void) {
    uint32_t value = 0;
    TEST_ASSERT_TRUE(mqttStrToU32(mqttStrFromCStr("007"), &value));
    TEST_ASSERT_EQUAL_UINT32(7, value);
}

void test_toU32_accepts_the_largest_value(void) {
    uint32_t value = 0;
    TEST_ASSERT_TRUE(mqttStrToU32(mqttStrFromCStr("4294967295"), &value));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, value);
}

void test_toU32_rejects_overflow(void) {
    uint32_t value = 0;
    TEST_ASSERT_FALSE(mqttStrToU32(mqttStrFromCStr("4294967296"), &value));
    TEST_ASSERT_FALSE(mqttStrToU32(mqttStrFromCStr("99999999999999999999"), &value));
}

void test_toU32_rejects_empty_input(void) {
    uint32_t value = 0;
    TEST_ASSERT_FALSE(mqttStrToU32(mqttStrFromCStr(""), &value));
    TEST_ASSERT_FALSE(mqttStrToU32(mqttStrEmpty(), &value));
}

void test_toU32_rejects_a_sign(void) {
    uint32_t value = 0;
    TEST_ASSERT_FALSE(mqttStrToU32(mqttStrFromCStr("+1"), &value));
    TEST_ASSERT_FALSE(mqttStrToU32(mqttStrFromCStr("-1"), &value));
}

void test_toU32_rejects_whitespace(void) {
    uint32_t value = 0;
    TEST_ASSERT_FALSE(mqttStrToU32(mqttStrFromCStr(" 1"), &value));
    TEST_ASSERT_FALSE(mqttStrToU32(mqttStrFromCStr("1 "), &value));
    TEST_ASSERT_FALSE(mqttStrToU32(mqttStrFromCStr("1\n"), &value));
}

/* A segment that starts with digits is not an index, and saying so is the
   difference between refusing "2abc" and routing it as 2. */
void test_toU32_rejects_a_partial_parse(void) {
    uint32_t value = 0;
    TEST_ASSERT_FALSE(mqttStrToU32(mqttStrFromCStr("2abc"), &value));
    TEST_ASSERT_FALSE(mqttStrToU32(mqttStrFromCStr("0x10"), &value));
}

void test_toU32_leaves_out_alone_on_failure(void) {
    uint32_t value = 77;
    TEST_ASSERT_FALSE(mqttStrToU32(mqttStrFromCStr("nope"), &value));
    TEST_ASSERT_EQUAL_UINT32(77, value);
}

void test_toU32_respects_view_bounds(void) {
    const char buffer[] = "12x";
    uint32_t value = 0;
    TEST_ASSERT_TRUE(mqttStrToU32(mqttStrFromBytes(buffer, 2), &value));
    TEST_ASSERT_EQUAL_UINT32(12, value);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_empty_is_empty);
    RUN_TEST(test_absent_is_not_present);
    RUN_TEST(test_empty_string_is_present);
    RUN_TEST(test_nonempty_is_present);
    RUN_TEST(test_fromCStr_null_is_absent);
    RUN_TEST(test_zero_length_view_over_a_buffer_is_present);

    RUN_TEST(test_fromCStr_length);
    RUN_TEST(test_fromCStr_does_not_copy);
    RUN_TEST(test_fromCStr_null_is_empty);
    RUN_TEST(test_fromCStr_empty_string);
    RUN_TEST(test_fromBytes_is_not_nul_terminated);

    RUN_TEST(test_eq_same_content);
    RUN_TEST(test_eq_different_content);
    RUN_TEST(test_eq_different_length);
    RUN_TEST(test_eq_respects_length_not_terminator);
    RUN_TEST(test_eq_empty_strings);

    RUN_TEST(test_startsWith_true);
    RUN_TEST(test_startsWith_false);
    RUN_TEST(test_startsWith_longer_prefix_than_string);
    RUN_TEST(test_startsWith_whole_string);
    RUN_TEST(test_startsWith_empty_prefix);
    RUN_TEST(test_prefix_matches_where_equality_does_not);

    RUN_TEST(test_toCStr_exact_fit);
    RUN_TEST(test_toCStr_terminates_a_view);
    RUN_TEST(test_toCStr_truncates_and_reports);
    RUN_TEST(test_toCStr_always_terminates);
    RUN_TEST(test_toCStr_zero_size_writes_nothing);
    RUN_TEST(test_toCStr_null_dst_is_safe);
    RUN_TEST(test_toCStr_empty_source);

    RUN_TEST(test_segments_simple);
    RUN_TEST(test_segments_single);
    RUN_TEST(test_segments_empty_topic_yields_none);
    RUN_TEST(test_segments_empty_middle);
    RUN_TEST(test_segments_trailing_separator);
    RUN_TEST(test_segments_leading_separator);
    RUN_TEST(test_segments_do_not_copy);
    RUN_TEST(test_segments_respect_view_bounds);
    RUN_TEST(test_segments_null_args_are_safe);
    RUN_TEST(test_segments_exhausted_stays_exhausted);

    RUN_TEST(test_strip_prefix_removes_prefix_and_separator);
    RUN_TEST(test_strip_prefix_matches_whole_segments);
    RUN_TEST(test_strip_prefix_of_the_whole_topic_leaves_nothing);
    RUN_TEST(test_strip_prefix_trailing_separator_leaves_nothing);
    RUN_TEST(test_strip_prefix_rejects_a_different_topic);
    RUN_TEST(test_strip_prefix_rejects_a_prefix_longer_than_the_topic);
    RUN_TEST(test_strip_prefix_empty_prefix_strips_nothing);
    RUN_TEST(test_strip_prefix_null_rest_is_a_predicate);
    RUN_TEST(test_strip_prefix_does_not_copy);

    RUN_TEST(test_match_literal_pattern);
    RUN_TEST(test_match_captures_a_plus);
    RUN_TEST(test_match_captures_in_pattern_order);
    RUN_TEST(test_match_rejects_an_unconsumed_trailing_segment);
    RUN_TEST(test_match_rejects_a_topic_that_runs_out);
    RUN_TEST(test_match_plus_captures_an_empty_segment);
    RUN_TEST(test_match_hash_captures_the_rest);
    RUN_TEST(test_match_hash_captures_one_segment);
    RUN_TEST(test_match_hash_captures_nothing_at_the_end);
    RUN_TEST(test_match_hash_alone_takes_the_whole_topic);
    RUN_TEST(test_match_rejects_a_hash_that_is_not_last);
    RUN_TEST(test_match_never_writes_past_maxCaptures);
    RUN_TEST(test_match_without_capturing);
    RUN_TEST(test_match_zeroes_the_count_on_a_failed_match);
    RUN_TEST(test_match_empty_pattern_matches_only_an_empty_topic);
    RUN_TEST(test_match_respects_view_bounds);
    RUN_TEST(test_match_captures_borrow_the_topic);

    RUN_TEST(test_matchUnder_strips_then_matches);
    RUN_TEST(test_matchUnder_rejects_a_partial_prefix_segment);
    RUN_TEST(test_matchUnder_rejects_the_rest_not_matching);
    RUN_TEST(test_matchUnder_no_prefix_is_a_plain_match);

    RUN_TEST(test_toU32_parses_digits);
    RUN_TEST(test_toU32_accepts_leading_zeros);
    RUN_TEST(test_toU32_accepts_the_largest_value);
    RUN_TEST(test_toU32_rejects_overflow);
    RUN_TEST(test_toU32_rejects_empty_input);
    RUN_TEST(test_toU32_rejects_a_sign);
    RUN_TEST(test_toU32_rejects_whitespace);
    RUN_TEST(test_toU32_rejects_a_partial_parse);
    RUN_TEST(test_toU32_leaves_out_alone_on_failure);
    RUN_TEST(test_toU32_respects_view_bounds);

    return UNITY_END();
}
