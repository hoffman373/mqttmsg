/* Tests for the shared reconnect policy (src/mqttmsg/backoff.c).

   Both clients pace their reconnects with this, and only one of them can be
   run on a laptop. Pinning the arithmetic here is what gives the device path
   any coverage at all — everything above this file on the Pico is lwIP
   callbacks and a WiFi chip. */

#include "unity/unity.h"
#include "backoff.h"

#define MIN_MS 1000u
#define MAX_MS 120000u
#define HEALTHY_MS 30000u

static ReconnectBackoff backoff;

void setUp(void) { backoffInit(&backoff, MIN_MS, MAX_MS, HEALTHY_MS); }

void tearDown(void) {}

/* Shorthand for an attempt that never reached a broker. */
static unsigned failed(void) { return backoffOnAttemptEnded(&backoff, false, 0); }

void test_first_wait_is_the_minimum(void) {
    /* Not twice it: the doubling belongs after the wait, or a broker that
       bounces cleanly is punished with a two-second gap. */
    TEST_ASSERT_EQUAL_UINT(1000, failed());
}

void test_waits_double(void) {
    TEST_ASSERT_EQUAL_UINT(1000, failed());
    TEST_ASSERT_EQUAL_UINT(2000, failed());
    TEST_ASSERT_EQUAL_UINT(4000, failed());
    TEST_ASSERT_EQUAL_UINT(8000, failed());
    TEST_ASSERT_EQUAL_UINT(16000, failed());
}

void test_waits_clamp_at_the_ceiling(void) {
    for (int i = 0; i < 6; i++) {
        (void)failed(); /* 1s 2s 4s 8s 16s 32s */
    }
    TEST_ASSERT_EQUAL_UINT(64000, failed());
    /* 128s would be the next double; the ceiling takes it instead. */
    TEST_ASSERT_EQUAL_UINT(120000, failed());
    TEST_ASSERT_EQUAL_UINT(120000, failed());
    TEST_ASSERT_EQUAL_UINT(120000, failed());
}

void test_healthy_session_resets_the_wait(void) {
    for (int i = 0; i < 5; i++) {
        (void)failed();
    }
    /* A session that reached the broker and lasted long enough. */
    TEST_ASSERT_EQUAL_UINT(1000, backoffOnAttemptEnded(&backoff, true, HEALTHY_MS));
    TEST_ASSERT_EQUAL_UINT(2000, failed());
}

void test_short_session_does_not_reset(void) {
    /* The case the duration test exists for: a broker that accepts the
       connection and drops it immediately would otherwise reset the wait on
       every cycle and never be paced at all. */
    (void)failed(); /* 1s */
    (void)failed(); /* 2s */
    TEST_ASSERT_EQUAL_UINT(4000, backoffOnAttemptEnded(&backoff, true, HEALTHY_MS - 1));
    TEST_ASSERT_EQUAL_UINT(8000, backoffOnAttemptEnded(&backoff, true, 0));
}

void test_long_session_without_a_handshake_does_not_reset(void) {
    /* A socket that stayed open for an hour without the broker ever
       accepting the CONNECT is not a working broker. */
    (void)failed();
    (void)failed();
    TEST_ASSERT_EQUAL_UINT(4000, backoffOnAttemptEnded(&backoff, false, 3600000));
}

void test_a_flapping_broker_still_ends_up_paced(void) {
    /* Ten accept-then-drop cycles: the wait has to climb anyway. */
    unsigned wait = 0;
    for (int i = 0; i < 10; i++) {
        wait = backoffOnAttemptEnded(&backoff, true, 1);
    }
    TEST_ASSERT_TRUE_MESSAGE(wait >= 120000 / 4, "a flapping broker escaped pacing");
}

void test_repeated_healthy_sessions_stay_at_the_minimum(void) {
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT(1000, backoffOnAttemptEnded(&backoff, true, HEALTHY_MS * 2));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_first_wait_is_the_minimum);
    RUN_TEST(test_waits_double);
    RUN_TEST(test_waits_clamp_at_the_ceiling);
    RUN_TEST(test_healthy_session_resets_the_wait);
    RUN_TEST(test_short_session_does_not_reset);
    RUN_TEST(test_long_session_without_a_handshake_does_not_reset);
    RUN_TEST(test_a_flapping_broker_still_ends_up_paced);
    RUN_TEST(test_repeated_healthy_sessions_stay_at_the_minimum);
    return UNITY_END();
}
