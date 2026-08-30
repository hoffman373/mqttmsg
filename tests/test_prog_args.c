#include "unity/unity.h"
#include "prog_args.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void setUp(void) {
    /* getopt uses global optind; reset before every test. */
    optind = 1;
}

void tearDown(void) {}

/* ── Defaults ─────────────────────────────────────────────────────── */

void test_defaults_address(void) {
    char *argv[] = {"prog"};
    ProgArgs args;
    mqttParseArgs(1, argv, &args);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", args.mqttServerAddress);
    free(args.subscriptions);
}

void test_defaults_port(void) {
    char *argv[] = {"prog"};
    ProgArgs args;
    mqttParseArgs(1, argv, &args);
    TEST_ASSERT_EQUAL_INT(1883, args.mqttServerPort);
    free(args.subscriptions);
}

void test_defaults_no_username(void) {
    char *argv[] = {"prog"};
    ProgArgs args;
    mqttParseArgs(1, argv, &args);
    TEST_ASSERT_NULL(args.username);
    free(args.subscriptions);
}

void test_defaults_no_password(void) {
    char *argv[] = {"prog"};
    ProgArgs args;
    mqttParseArgs(1, argv, &args);
    TEST_ASSERT_NULL(args.password);
    free(args.subscriptions);
}

void test_defaults_no_subscriptions(void) {
    char *argv[] = {"prog"};
    ProgArgs args;
    mqttParseArgs(1, argv, &args);
    TEST_ASSERT_EQUAL_INT(0, args.numSubscriptions);
    free(args.subscriptions);
}

/* ── Address (-a) ─────────────────────────────────────────────────── */

void test_address_flag(void) {
    char *argv[] = {"prog", "-a", "192.168.1.50"};
    ProgArgs args;
    mqttParseArgs(3, argv, &args);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", args.mqttServerAddress);
    free(args.subscriptions);
}

/* ── Port (-p) ────────────────────────────────────────────────────── */

void test_port_flag(void) {
    char *argv[] = {"prog", "-p", "8883"};
    ProgArgs args;
    mqttParseArgs(3, argv, &args);
    TEST_ASSERT_EQUAL_INT(8883, args.mqttServerPort);
    free(args.subscriptions);
}

void test_port_flag_non_default(void) {
    char *argv[] = {"prog", "-p", "1234"};
    ProgArgs args;
    mqttParseArgs(3, argv, &args);
    TEST_ASSERT_EQUAL_INT(1234, args.mqttServerPort);
    free(args.subscriptions);
}

/* ── Username (-u) ────────────────────────────────────────────────── */

void test_username_flag(void) {
    char *argv[] = {"prog", "-u", "alice"};
    ProgArgs args;
    mqttParseArgs(3, argv, &args);
    TEST_ASSERT_EQUAL_STRING("alice", args.username);
    free(args.username);
    free(args.subscriptions);
}

/* ── Password (-m) ────────────────────────────────────────────────── */

void test_password_flag(void) {
    char *argv[] = {"prog", "-m", "s3cr3t"};
    ProgArgs args;
    mqttParseArgs(3, argv, &args);
    TEST_ASSERT_EQUAL_STRING("s3cr3t", args.password);
    free(args.password);
    free(args.subscriptions);
}

/* ── Client id (-i) ───────────────────────────────────────────────── */

void test_client_id_flag(void) {
    char *argv[] = {"prog", "-i", "kitchen-pico"};
    ProgArgs args;
    mqttParseArgs(3, argv, &args);
    TEST_ASSERT_EQUAL_STRING("kitchen-pico", args.clientId);
    free(args.subscriptions);
}

void test_defaults_no_client_id(void) {
    /* NULL is what tells the caller to generate one, so the default has to
       stay NULL rather than becoming an empty string. */
    char *argv[] = {"prog"};
    ProgArgs args;
    mqttParseArgs(1, argv, &args);
    TEST_ASSERT_NULL(args.clientId);
    free(args.subscriptions);
}

void test_client_id_is_borrowed_from_argv(void) {
    /* Not copied, unlike the credentials: the pointer is argv's, which is
       why nothing frees it. A copy here would leak on every parse. */
    char *argv[] = {"prog", "-i", "kitchen-pico"};
    ProgArgs args;
    mqttParseArgs(3, argv, &args);
    TEST_ASSERT_EQUAL_PTR(argv[2], args.clientId);
    free(args.subscriptions);
}

/* ── Subscriptions (-s) ───────────────────────────────────────────── */

void test_single_subscription(void) {
    char *argv[] = {"prog", "-s", "home/temp"};
    ProgArgs args;
    mqttParseArgs(3, argv, &args);
    TEST_ASSERT_EQUAL_INT(1, args.numSubscriptions);
    TEST_ASSERT_EQUAL_STRING("home/temp", args.subscriptions[0]);
    free(args.subscriptions);
}

void test_multiple_subscriptions_count(void) {
    char *argv[] = {"prog", "-s", "a/b", "-s", "c/d", "-s", "e/f"};
    ProgArgs args;
    mqttParseArgs(7, argv, &args);
    TEST_ASSERT_EQUAL_INT(3, args.numSubscriptions);
    free(args.subscriptions);
}

void test_multiple_subscriptions_values(void) {
    char *argv[] = {"prog", "-s", "a/b", "-s", "c/d"};
    ProgArgs args;
    mqttParseArgs(5, argv, &args);
    TEST_ASSERT_EQUAL_STRING("a/b", args.subscriptions[0]);
    TEST_ASSERT_EQUAL_STRING("c/d", args.subscriptions[1]);
    free(args.subscriptions);
}

void test_subscriptions_null_terminated(void) {
    char *argv[] = {"prog", "-s", "a/b"};
    ProgArgs args;
    mqttParseArgs(3, argv, &args);
    /* calloc allocates numSubscriptions+1 slots, last must be NULL */
    TEST_ASSERT_NULL(args.subscriptions[1]);
    free(args.subscriptions);
}

/* ── Combined flags ───────────────────────────────────────────────── */

void test_combined_flags(void) {
    char *argv[] = {"prog", "-a", "10.0.0.1", "-p", "9999",        "-u",
                    "bob",  "-m", "pass",     "-i", "kitchen-pico"};
    ProgArgs args;
    mqttParseArgs(11, argv, &args);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", args.mqttServerAddress);
    TEST_ASSERT_EQUAL_INT(9999, args.mqttServerPort);
    TEST_ASSERT_EQUAL_STRING("bob", args.username);
    TEST_ASSERT_EQUAL_STRING("pass", args.password);
    TEST_ASSERT_EQUAL_STRING("kitchen-pico", args.clientId);
    free(args.username);
    free(args.password);
    free(args.subscriptions);
}

/* ── Error / help cases ───────────────────────────────────────────── */

void test_help_flag_returns_zero(void) {
    char *argv[] = {"prog", "-h"};
    ProgArgs args;
    int result = mqttParseArgs(2, argv, &args);
    TEST_ASSERT_EQUAL_INT(0, result);
}

void test_unknown_flag_returns_zero(void) {
    char *argv[] = {"prog", "-z"};
    ProgArgs args;
    int result = mqttParseArgs(2, argv, &args);
    TEST_ASSERT_EQUAL_INT(0, result);
}

void test_success_returns_one(void) {
    char *argv[] = {"prog", "-a", "localhost"};
    ProgArgs args;
    int result = mqttParseArgs(3, argv, &args);
    TEST_ASSERT_EQUAL_INT(1, result);
    free(args.subscriptions);
}

/* ── mqttParseArgsWithDefaults preserves pre-set values ──────────────── */

void test_with_defaults_preserves_address(void) {
    char *argv[] = {"prog", "-p", "9000"};
    ProgArgs args;
    memset(&args, 0, sizeof(ProgArgs));
    args.mqttServerAddress = "mqtt.example.com";
    args.mqttServerPort = 1883;
    mqttParseArgsWithDefaults(3, argv, &args);
    TEST_ASSERT_EQUAL_STRING("mqtt.example.com", args.mqttServerAddress);
    free(args.subscriptions);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_defaults_address);
    RUN_TEST(test_defaults_port);
    RUN_TEST(test_defaults_no_username);
    RUN_TEST(test_defaults_no_password);
    RUN_TEST(test_defaults_no_subscriptions);

    RUN_TEST(test_address_flag);

    RUN_TEST(test_port_flag);
    RUN_TEST(test_port_flag_non_default);

    RUN_TEST(test_username_flag);
    RUN_TEST(test_password_flag);

    RUN_TEST(test_client_id_flag);
    RUN_TEST(test_defaults_no_client_id);
    RUN_TEST(test_client_id_is_borrowed_from_argv);

    RUN_TEST(test_single_subscription);
    RUN_TEST(test_multiple_subscriptions_count);
    RUN_TEST(test_multiple_subscriptions_values);
    RUN_TEST(test_subscriptions_null_terminated);

    RUN_TEST(test_combined_flags);

    RUN_TEST(test_help_flag_returns_zero);
    RUN_TEST(test_unknown_flag_returns_zero);
    RUN_TEST(test_success_returns_one);

    RUN_TEST(test_with_defaults_preserves_address);

    return UNITY_END();
}
