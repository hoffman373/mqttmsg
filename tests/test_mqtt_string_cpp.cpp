/* C++ interop tests for mqtt_string.hpp.

   Half of what this file checks is that it compiles at all: the constexpr
   assertions below are evaluated by the compiler, and the extern "C" block
   here reproduces how consumer projects include the C headers. */

extern "C" {
#include <mqttmsg/mqtt_string.h>
#include <mqttmsg/mqtt_types.h>
#include "unity/unity.h"
}

/* Deliberately not inside an extern "C" block: a public C header carries its
   own guard, and keymanager.h once did not, so a C++ consumer got mangled
   names for functions compiled as C and the link failed. Calling
   mqttCheckKey() below is what makes that a link error rather than a
   silently unused declaration. (makeNonce() is device-only, so there is
   nothing on the host to link it against.) */
#include <mqttmsg/keymanager.h>

/* Outside the extern "C" block, as the headers require. */
#include <mqttmsg/mqtt_message.hpp>
#include <mqttmsg/mqtt_string.hpp>

#include <string>
#include <string_view>
#include <vector>

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

/* ── Compile-time contract ────────────────────────────────────────── */

/* Comparisons route through std::string_view, so they fold at compile time.
   If these stop being constexpr the header has regressed. */
static_assert(mqttmsg::view(MqttString{"abc", 3}) == "abc");
static_assert(MqttString{"abc", 3} == std::string_view("abc"));
static_assert(!(MqttString{"abc", 3} == std::string_view("abd")));
static_assert(mqttmsg::startsWith(MqttString{"atx/1", 5}, "atx/"));
static_assert(*mqttmsg::stripTopicPrefix(MqttString{"atx/1", 5}, "atx") == std::string_view("1"));
static_assert(!mqttmsg::stripTopicPrefix(MqttString{"atxfoo/1", 8}, "atx").has_value());
static_assert(mqttmsg::fromView(std::string_view("hello")).length == 5);

/* The aggregate must survive contact with the C++ header. */
static_assert(std::is_aggregate_v<MqttString>, "MqttString must stay brace-initializable");

/* ── string_view interop ──────────────────────────────────────────── */

void test_view_matches_content(void) {
    MqttString s = mqttStrFromCStr("home/temp");
    TEST_ASSERT_TRUE(mqttmsg::view(s) == std::string_view("home/temp"));
}

void test_view_does_not_copy(void) {
    const char* original = "home/temp";
    std::string_view v = mqttmsg::view(mqttStrFromCStr(original));
    TEST_ASSERT_EQUAL_PTR(original, v.data());
}

void test_view_of_empty_is_empty(void) { TEST_ASSERT_TRUE(mqttmsg::view(mqttStrEmpty()).empty()); }

void test_view_respects_length_not_terminator(void) {
    const char buffer[] = "home/tempGARBAGE";
    std::string_view v = mqttmsg::view(mqttStrFromBytes(buffer, 9));
    TEST_ASSERT_EQUAL_size_t(9, v.size());
    TEST_ASSERT_TRUE(v == "home/temp");
}

void test_roundtrip_through_view(void) {
    MqttString s = mqttmsg::fromView(std::string_view("a/b/c"));
    TEST_ASSERT_TRUE(mqttStrEqCStr(s, "a/b/c"));
}

/* ── Comparison operators ─────────────────────────────────────────── */

void test_compare_against_literal(void) {
    MqttString topic = mqttStrFromCStr("atx/1/command");
    TEST_ASSERT_TRUE(topic == "atx/1/command");
    TEST_ASSERT_FALSE(topic == "atx/2/command");
}

void test_compare_reversed_operands(void) {
    /* Under C++20 this resolves through the synthesized reversed candidate;
       if both directions were declared it would be ambiguous instead. */
    MqttString topic = mqttStrFromCStr("atx");
    TEST_ASSERT_TRUE(std::string_view("atx") == topic);
}

void test_compare_two_views(void) {
    TEST_ASSERT_TRUE(mqttStrFromCStr("a/b") == mqttStrFromCStr("a/b"));
    TEST_ASSERT_FALSE(mqttStrFromCStr("a/b") == mqttStrFromCStr("a/c"));
}

void test_inequality(void) {
    MqttString topic = mqttStrFromCStr("atx");
    TEST_ASSERT_TRUE(topic != "other");
}

void test_compare_against_std_string(void) {
    std::string expected = "home/temp";
    TEST_ASSERT_TRUE(mqttStrFromCStr("home/temp") == std::string_view(expected));
}

/* ── Range-for over segments ──────────────────────────────────────── */

void test_range_for_segments(void) {
    std::vector<std::string> got;
    for (MqttString segment : mqttmsg::segments(mqttStrFromCStr("atx/1/command"))) {
        got.emplace_back(mqttmsg::view(segment));
    }

    TEST_ASSERT_EQUAL_INT(3, (int)got.size());
    TEST_ASSERT_TRUE(got[0] == "atx");
    TEST_ASSERT_TRUE(got[1] == "1");
    TEST_ASSERT_TRUE(got[2] == "command");
}

void test_range_for_empty_topic(void) {
    int count = 0;
    for (MqttString segment : mqttmsg::segments(mqttStrEmpty())) {
        (void)segment;
        count++;
    }
    TEST_ASSERT_EQUAL_INT(0, count);
}

void test_range_for_can_break_early(void) {
    /* A visitor callback could not do this, which is why the C API is a
       cursor. */
    int count = 0;
    for (MqttString segment : mqttmsg::segments(mqttStrFromCStr("a/b/c/d"))) {
        count++;
        if (segment == "b") {
            break;
        }
    }
    TEST_ASSERT_EQUAL_INT(2, count);
}

void test_segments_alias_original_buffer(void) {
    MqttString topic = mqttStrFromCStr("atx/1");
    for (MqttString segment : mqttmsg::segments(topic)) {
        TEST_ASSERT_EQUAL_PTR(topic.data, segment.data);
        break;
    }
}

/* The realistic consumer pattern: pico-atx-control's onCommand parses
   "<prefix>/<index>/command" out of the topic — a job sscanf cannot do,
   since the view has no NUL terminator for it. */
void test_parse_command_topic(void) {
    MqttString topic = mqttStrFromCStr("atx/3/command");

    int index = -1;
    bool isCommand = false;
    int position = 0;
    for (MqttString segment : mqttmsg::segments(topic)) {
        if (position == 1) {
            char buf[8] = {};
            mqttStrToCStr(buf, sizeof(buf), segment);
            index = atoi(buf);
        } else if (position == 2) {
            isCommand = (segment == "command");
        }
        position++;
    }

    TEST_ASSERT_EQUAL_INT(3, index);
    TEST_ASSERT_TRUE(isCommand);
}

/* ── Prefix stripping ─────────────────────────────────────────────── */

void test_strip_topic_prefix(void) {
    auto rest = mqttmsg::stripTopicPrefix(mqttStrFromCStr("atx/3/command"), "atx");
    TEST_ASSERT_TRUE(rest.has_value());
    TEST_ASSERT_TRUE(*rest == "3/command");
}

void test_strip_topic_prefix_wants_a_whole_segment(void) {
    TEST_ASSERT_FALSE(
        mqttmsg::stripTopicPrefix(mqttStrFromCStr("atxfoo/3/command"), "atx").has_value());
}

void test_strip_topic_prefix_of_the_whole_topic(void) {
    auto rest = mqttmsg::stripTopicPrefix(mqttStrFromCStr("atx"), "atx");
    TEST_ASSERT_TRUE(rest.has_value());
    TEST_ASSERT_TRUE(mqttStrIsEmpty(*rest));
}

void test_strip_topic_prefix_empty_prefix_strips_nothing(void) {
    auto rest = mqttmsg::stripTopicPrefix(mqttStrFromCStr("atx/3"), "");
    TEST_ASSERT_TRUE(rest.has_value());
    TEST_ASSERT_TRUE(*rest == "atx/3");
}

void test_strip_topic_prefix_does_not_copy(void) {
    const char* topic = "atx/3/command";
    auto rest = mqttmsg::stripTopicPrefix(mqttStrFromCStr(topic), "atx");
    TEST_ASSERT_TRUE(rest.has_value());
    TEST_ASSERT_EQUAL_PTR(topic + 4, rest->data);
}

/* The same routing as test_parse_command_topic, with the pieces this
   library provides rather than a segment counter and an atoi(). */
void test_route_a_command_topic(void) {
    MqttString captures[1];
    uint32_t index = 0;
    TEST_ASSERT_TRUE(mqttTopicMatchUnder(mqttStrFromCStr("atx/3/command"), "atx", "+/command",
                                         captures, 1, nullptr));
    TEST_ASSERT_TRUE(mqttStrToU32(captures[0], &index));
    TEST_ASSERT_EQUAL_UINT32(3, index);
}

/* ── MqttPayload span ─────────────────────────────────────────────────── */

#ifdef MQTTMSG_HAS_SPAN
void test_payload_span(void) {
    uint8_t raw[] = {'s', 'e', 'n', 's', 'o', 'r'};
    MqttPayload p = {raw, 6};
    std::span<const uint8_t> s = mqttmsg::bytes(p);
    TEST_ASSERT_EQUAL_size_t(6, s.size());
    TEST_ASSERT_EQUAL_PTR(raw, s.data());
}

void test_payload_span_empty(void) {
    MqttPayload p = {nullptr, 0};
    TEST_ASSERT_TRUE(mqttmsg::bytes(p).empty());
}
#endif

/* ── Message accessors as optionals ───────────────────────────────── */

void test_optional_connect_payload(void) {
    MqttPayload p = buildConnect("cid", "alice", "s3cr3t", nullptr, nullptr, false, 90);
    FixedHeader fh = parseFixedHeader(p);
    Message m = parseMessage(fh, p);

    auto connect = mqttmsg::connectPayload(m);
    TEST_ASSERT_TRUE(connect.has_value());
    TEST_ASSERT_TRUE(connect->clientId == "cid");
    TEST_ASSERT_TRUE(connect->username == "alice");
    mqttPayloadFree(&p);
}

void test_optional_is_empty_for_wrong_type(void) {
    MqttPayload p = buildPingReq();
    FixedHeader fh = parseFixedHeader(p);
    Message m = parseMessage(fh, p);
    TEST_ASSERT_FALSE(mqttmsg::connectPayload(m).has_value());
    TEST_ASSERT_FALSE(mqttmsg::subscriptions(m).has_value());
    TEST_ASSERT_FALSE(mqttmsg::grantedQos(m).has_value());
    mqttPayloadFree(&p);
}

void test_optional_binds_check_and_value(void) {
    /* The shape the header exists for: one call, and the value cannot be
       reached without the check succeeding. */
    MqttPayload p = buildConnect("abc", nullptr, nullptr, nullptr, nullptr, false, 90);
    FixedHeader fh = parseFixedHeader(p);
    Message m = parseMessage(fh, p);

    std::string got;
    if (auto connect = mqttmsg::connectPayload(m)) {
        got = std::string(mqttmsg::view(connect->clientId));
    }
    TEST_ASSERT_TRUE(got == "abc");
    mqttPayloadFree(&p);
}

void test_range_for_over_subscriptions(void) {
    Subscription subs[2] = {
        {"a/b", AtMostOnce},
        {"c/d", AtLeastOnce},
    };
    MqttPayload p = buildSubscribe(1, subs, 2);
    FixedHeader fh = parseFixedHeader(p);
    Message m = parseMessage(fh, p);

    auto payload = mqttmsg::subscriptions(m);
    TEST_ASSERT_TRUE(payload.has_value());

    std::vector<std::string> topics;
    for (MqttSubscription s : mqttmsg::each(*payload)) {
        topics.emplace_back(mqttmsg::view(s.topic));
    }

    TEST_ASSERT_EQUAL_INT(2, (int)topics.size());
    TEST_ASSERT_TRUE(topics[0] == "a/b");
    TEST_ASSERT_TRUE(topics[1] == "c/d");
    mqttPayloadFree(&p);
}

/* ── Main ─────────────────────────────────────────────────────────── */

/* ── C linkage of the public headers ──────────────────────────────── */

static void test_keymanager_header_links_from_cpp(void) {
    /* The verification itself is covered by test_message_auth.c against
       golden vectors. All this needs is a real call, so that a keymanager.h
       without its extern "C" guard fails to link this file. */
    MqttPayload message = mqttPayloadNone();
    uint8_t hash[32] = {};

    TEST_ASSERT_FALSE(mqttCheckKey(message, "key", (const uint8_t*)"nonce", 5, hash));
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_view_matches_content);
    RUN_TEST(test_view_does_not_copy);
    RUN_TEST(test_view_of_empty_is_empty);
    RUN_TEST(test_view_respects_length_not_terminator);
    RUN_TEST(test_roundtrip_through_view);

    RUN_TEST(test_compare_against_literal);
    RUN_TEST(test_compare_reversed_operands);
    RUN_TEST(test_compare_two_views);
    RUN_TEST(test_inequality);
    RUN_TEST(test_compare_against_std_string);

    RUN_TEST(test_range_for_segments);
    RUN_TEST(test_range_for_empty_topic);
    RUN_TEST(test_range_for_can_break_early);
    RUN_TEST(test_segments_alias_original_buffer);
    RUN_TEST(test_parse_command_topic);

    RUN_TEST(test_strip_topic_prefix);
    RUN_TEST(test_strip_topic_prefix_wants_a_whole_segment);
    RUN_TEST(test_strip_topic_prefix_of_the_whole_topic);
    RUN_TEST(test_strip_topic_prefix_empty_prefix_strips_nothing);
    RUN_TEST(test_strip_topic_prefix_does_not_copy);
    RUN_TEST(test_route_a_command_topic);

    RUN_TEST(test_optional_connect_payload);
    RUN_TEST(test_optional_is_empty_for_wrong_type);
    RUN_TEST(test_optional_binds_check_and_value);
    RUN_TEST(test_range_for_over_subscriptions);

#ifdef MQTTMSG_HAS_SPAN
    RUN_TEST(test_keymanager_header_links_from_cpp);

    RUN_TEST(test_payload_span);
    RUN_TEST(test_payload_span_empty);
#endif

    return UNITY_END();
}
