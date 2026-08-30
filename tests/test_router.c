/* Tests for topic-to-subscription routing (src/mqttmsg/router.c).

   This decides which of a client's registered callbacks a received message
   reaches, and a miss here is the quietest failure the library has: the
   broker delivers the messages, the client holds a subscription for them,
   the callback never runs, and nothing logs.

   The matcher itself is mqttTopicMatch(), which has its own tests in
   test_mqtt_string.c. What is checked here is the routing built on it —
   that the filter vocabulary is honoured, and that a topic reaches every
   subscription covering it rather than only the first. */

#include "unity/unity.h"
#include "router.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static bool matches(const char* filter, const char* topic) {
    return mqttRouteMatches(mqttStrFromCStr(topic), filter);
}

/* --- the vocabulary ------------------------------------------------- */

static void test_exactFilterMatchesOnlyThatTopic(void) {
    TEST_ASSERT_TRUE(matches("atx/1/command", "atx/1/command"));
    TEST_ASSERT_FALSE(matches("atx/1/command", "atx/2/command"));
}

static void test_exactFilterIsNotAPrefix(void) {
    /* The regression that motivated the previous rewrite: "atx/1" must not
       fire for "atx/10". */
    TEST_ASSERT_FALSE(matches("atx/1", "atx/10"));
}

static void test_plusMatchesOneLevel(void) {
    /* The case a matcher that only understands a trailing '#' gets wrong:
       falling through to an exact compare here matches nothing, silently. */
    TEST_ASSERT_TRUE(matches("atx/+/command", "atx/2/command"));
    TEST_ASSERT_TRUE(matches("atx/+/command", "atx/17/command"));
}

static void test_plusDoesNotSpanLevels(void) {
    TEST_ASSERT_FALSE(matches("atx/+/command", "atx/2/sub/command"));
    TEST_ASSERT_FALSE(matches("atx/+", "atx/2/command"));
}

static void test_hashMatchesTheRest(void) {
    TEST_ASSERT_TRUE(matches("a/#", "a/b"));
    TEST_ASSERT_TRUE(matches("a/#", "a/b/c/d"));
}

static void test_hashMatchesTheParentTopic(void) {
    /* MQTT says "a/#" covers "a" itself — the case a prefix compare against
       "a/" gets wrong, since "a" does not start with it. */
    TEST_ASSERT_TRUE(matches("a/#", "a"));
}

static void test_hashDoesNotMatchASibling(void) {
    TEST_ASSERT_FALSE(matches("a/#", "ab/c"));
    TEST_ASSERT_FALSE(matches("a/#", "b/c"));
}

/* --- the walk -------------------------------------------------------- */

#define MAX_FILTERS 8

typedef struct {
    const char* filters[MAX_FILTERS];
    int count;
    int visited[MAX_FILTERS];
    int visitCount;
    char lastTopic[64];
} Table;

static const char* filterAt(void* ctx, int index) { return ((Table*)ctx)->filters[index]; }

static void record(void* ctx, int index, MqttString topic) {
    Table* t = (Table*)ctx;
    t->visited[t->visitCount++] = index;
    snprintf(t->lastTopic, sizeof(t->lastTopic), MQTT_STR_FMT, MQTT_STR_ARG(topic));
}

static void dispatchOver(Table* t, const char* topic) {
    t->visitCount = 0;
    mqttRouteDispatch(mqttStrFromCStr(topic), filterAt, record, t, t->count);
}

static void test_everyMatchingSubscriptionIsVisited(void) {
    /* Subscribing to both a specific topic and a wildcard covering it is a
       reasonable thing to do, and the broker sends the message once. Both
       callbacks are entitled to it. */
    Table t = {.filters = {"atx/1/command", "atx/#", "other/#"}, .count = 3};

    dispatchOver(&t, "atx/1/command");

    TEST_ASSERT_EQUAL_INT(2, t.visitCount);
    TEST_ASSERT_EQUAL_INT(0, t.visited[0]);
    TEST_ASSERT_EQUAL_INT(1, t.visited[1]);
}

static void test_nonMatchingSubscriptionsAreSkipped(void) {
    Table t = {.filters = {"a/#", "b/#"}, .count = 2};

    dispatchOver(&t, "b/thing");

    TEST_ASSERT_EQUAL_INT(1, t.visitCount);
    TEST_ASSERT_EQUAL_INT(1, t.visited[0]);
}

static void test_visitorSeesTheTopicThatArrived(void) {
    /* Not the filter — the callback needs to know which of the topics its
       wildcard covers actually turned up. */
    Table t = {.filters = {"atx/+/command"}, .count = 1};

    dispatchOver(&t, "atx/3/command");

    TEST_ASSERT_EQUAL_INT(1, t.visitCount);
    TEST_ASSERT_EQUAL_STRING("atx/3/command", t.lastTopic);
}

static void test_anEmptyTableVisitsNothing(void) {
    Table t = {.count = 0};

    dispatchOver(&t, "anything");

    TEST_ASSERT_EQUAL_INT(0, t.visitCount);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_exactFilterMatchesOnlyThatTopic);
    RUN_TEST(test_exactFilterIsNotAPrefix);
    RUN_TEST(test_plusMatchesOneLevel);
    RUN_TEST(test_plusDoesNotSpanLevels);
    RUN_TEST(test_hashMatchesTheRest);
    RUN_TEST(test_hashMatchesTheParentTopic);
    RUN_TEST(test_hashDoesNotMatchASibling);

    RUN_TEST(test_everyMatchingSubscriptionIsVisited);
    RUN_TEST(test_nonMatchingSubscriptionsAreSkipped);
    RUN_TEST(test_visitorSeesTheTopicThatArrived);
    RUN_TEST(test_anEmptyTableVisitsNothing);

    return UNITY_END();
}
