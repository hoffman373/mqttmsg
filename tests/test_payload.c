/* The MqttPayload contract: three states, and what every entry point does when
   handed the one that is not a frame.

   The encoding these exercise is deliberately not visible here — the point
   of the tests is that a caller only ever needs the constructors, the
   predicates and the accessors. The one place the representation appears is
   test_untagged_failure_is_not_decoded, which builds a MqttPayload the library
   itself would not, to prove a stale length is not read as a reason. */

#include "unity/unity.h"
#include <mqttmsg/mqtt.h>
#include <mqttmsg/mqtt_types.h>
#include <stdlib.h>
#include <string.h>

/* MqttPayload must stay two words. The failure states are carried inside the
   fields that were already there precisely so this stays true: 8 bytes on
   the Cortex-M33, 16 on the host, and no ABI break. */
_Static_assert(sizeof(MqttPayload) <= 2 * sizeof(void*), "MqttPayload has grown past two words");

/* ── Forced allocation failure ────────────────────────────────────────
   Linked with -Wl,--wrap=malloc, so the encoders' allocations come through
   here and can be made to fail on demand. Without this the out-of-memory
   paths are unreachable and would only ever be exercised on a device that
   was already in trouble. */

static int mallocFailIn = -1; /* Fail the Nth malloc from now; -1 disables. */

void* __real_malloc(size_t size);

void* __wrap_malloc(size_t size) {
    if (mallocFailIn == 0) {
        mallocFailIn = -1;
        return NULL;
    }

    if (mallocFailIn > 0) {
        mallocFailIn--;
    }

    return __real_malloc(size);
}

void setUp(void) { mallocFailIn = -1; }

void tearDown(void) { mallocFailIn = -1; }

/* ── The three states ─────────────────────────────────────────────────── */

void test_none_is_not_ok_not_empty_not_failure(void) {
    MqttPayload p = mqttPayloadNone();
    TEST_ASSERT_TRUE(mqttPayloadIsNone(p));
    TEST_ASSERT_FALSE(mqttPayloadIsOk(p));
    TEST_ASSERT_FALSE(mqttPayloadIsEmpty(p));
    TEST_ASSERT_FALSE(mqttPayloadIsFailure(p));
    TEST_ASSERT_EQUAL_UINT32(PayloadNoFailure, mqttPayloadReason(p));
}

void test_failure_is_only_a_failure(void) {
    MqttPayload p = mqttPayloadFailure(PayloadFailOom);
    TEST_ASSERT_TRUE(mqttPayloadIsFailure(p));
    TEST_ASSERT_FALSE(mqttPayloadIsOk(p));
    TEST_ASSERT_FALSE(mqttPayloadIsNone(p));
    /* Not empty: an earlier design made isEmpty true for failures and needed
       a documented check-ordering rule to be safe. */
    TEST_ASSERT_FALSE(mqttPayloadIsEmpty(p));
    TEST_ASSERT_EQUAL_UINT32(PayloadFailOom, mqttPayloadReason(p));
}

void test_ok_payload_carries_its_bytes(void) {
    uint8_t bytes[] = {1, 2, 3};
    MqttPayload p = mqttPayloadFromBytes(bytes, sizeof(bytes));
    TEST_ASSERT_TRUE(mqttPayloadIsOk(p));
    TEST_ASSERT_FALSE(mqttPayloadIsEmpty(p));
    TEST_ASSERT_EQUAL_UINT32(3, mqttPayloadLength(p));
    TEST_ASSERT_EQUAL_UINT8(2, mqttPayloadBytes(p)[1]);
}

void test_empty_is_present_with_no_bytes(void) {
    uint8_t bytes[] = {0};
    MqttPayload p = mqttPayloadFromBytes(bytes, 0);
    TEST_ASSERT_TRUE(mqttPayloadIsOk(p));
    TEST_ASSERT_TRUE(mqttPayloadIsEmpty(p));
    TEST_ASSERT_FALSE(mqttPayloadIsNone(p));
    TEST_ASSERT_FALSE(mqttPayloadIsFailure(p));
}

/* ── The tag is never visible as a length ─────────────────────────────── */

void test_failure_has_no_length_and_no_bytes(void) {
    MqttPayload p = mqttPayloadFailure(PayloadFailOverflow);
    TEST_ASSERT_EQUAL_UINT32(0, mqttPayloadLength(p));
    TEST_ASSERT_NULL(mqttPayloadBytes(p));
}

/* A length left behind by an allocation that never happened — what
   makeStringPayload("21.5") produces today when malloc fails. It is a
   failure, but it is not reason 4: decoding it would give a confidently
   wrong diagnosis, which is why the tags live at the top of the range. */
void test_untagged_failure_is_not_decoded(void) {
    MqttPayload stale = {NULL, 4};
    TEST_ASSERT_TRUE(mqttPayloadIsFailure(stale));
    TEST_ASSERT_FALSE(mqttPayloadIsNone(stale));
    TEST_ASSERT_EQUAL_UINT32(PayloadFailUnknown, mqttPayloadReason(stale));
    TEST_ASSERT_EQUAL_UINT32(0, mqttPayloadLength(stale));
    TEST_ASSERT_NULL(mqttPayloadBytes(stale));
}

/* A reason outside the reserved range cannot be stored as one, so it is
   recorded as unknown rather than becoming a length. */
void test_failure_constructor_rejects_a_bogus_reason(void) {
    MqttPayload p = mqttPayloadFailure((PayloadReason)7);
    TEST_ASSERT_TRUE(mqttPayloadIsFailure(p));
    TEST_ASSERT_EQUAL_UINT32(PayloadFailUnknown, mqttPayloadReason(p));
}

/* ── Freeing ──────────────────────────────────────────────────────────── */

void test_free_is_safe_on_every_state(void) {
    MqttPayload ok = makeStringPayload("body");
    mqttPayloadFree(&ok);
    TEST_ASSERT_TRUE(mqttPayloadIsNone(ok));
    /* Freeing twice is what the 85 hand-written free() calls could not
       promise; after the first call there is nothing left to release. */
    mqttPayloadFree(&ok);
    TEST_ASSERT_TRUE(mqttPayloadIsNone(ok));

    MqttPayload none = mqttPayloadNone();
    mqttPayloadFree(&none);
    TEST_ASSERT_TRUE(mqttPayloadIsNone(none));

    MqttPayload failed = mqttPayloadFailure(PayloadFailOom);
    mqttPayloadFree(&failed);
    TEST_ASSERT_TRUE(mqttPayloadIsNone(failed));
    TEST_ASSERT_FALSE(mqttPayloadIsFailure(failed));

    mqttPayloadFree(NULL);
}

/* ── What the builders return ─────────────────────────────────────────── */

void test_every_builder_is_ok_on_success(void) {
    Subscription subs[1] = {{.topic = "a/b", .qos = AtMostOnce}};
    const char* topics[] = {"a/b"};
    MqttPayload body = makeStringPayload("v");

    MqttPayload built[] = {
        mqttMakeString("s"),
        body,
        buildConnect("cid", NULL, NULL, NULL, NULL, false, 90),
        buildPublish("t", 1, body, false),
        buildPublishAck(1),
        buildPubRec(1),
        buildPubRel(1),
        buildPubComp(1),
        buildSubscribe(1, subs, 1),
        buildUnsubscribe(1, topics, 1),
        buildPingReq(),
        buildDisconnectMsg(),
    };

    for (size_t i = 0; i < sizeof(built) / sizeof(built[0]); i++) {
        char message[64];
        snprintf(message, sizeof(message), "builder %zu", i);
        TEST_ASSERT_TRUE_MESSAGE(mqttPayloadIsOk(built[i]), message);
        TEST_ASSERT_FALSE_MESSAGE(mqttPayloadIsFailure(built[i]), message);
        /* body is aliased into the array; freeing it once is enough. */
        if (i != 1) {
            mqttPayloadFree(&built[i]);
        }
    }

    mqttPayloadFree(&body);
}

void test_builder_reports_out_of_memory_as_a_failure(void) {
    mallocFailIn = 0;
    MqttPayload p = buildPingReq();
    TEST_ASSERT_TRUE(mqttPayloadIsFailure(p));
    TEST_ASSERT_FALSE(mqttPayloadIsOk(p));
    TEST_ASSERT_EQUAL_UINT32(PayloadFailOom, mqttPayloadReason(p));
    mqttPayloadFree(&p);
}

/* The frame allocation, rather than the remaining-length one: a different
   path out of the same function, and it must not leak the length buffer. */
void test_builder_reports_a_failed_frame_allocation(void) {
    mallocFailIn = 1;
    MqttPayload p = buildDisconnectMsg();
    TEST_ASSERT_TRUE(mqttPayloadIsFailure(p));
    TEST_ASSERT_EQUAL_UINT32(PayloadFailOom, mqttPayloadReason(p));
    mqttPayloadFree(&p);
}

/* Each constructor's own allocation, one at a time. mqttMakeString() is the
   subtle one: before the writer knew whether it was measuring, a failed
   allocation here produced a MqttPayload claiming strlen + 2 bytes at address
   zero, with every write silently discarded and nothing reporting it. */
void test_string_constructors_report_out_of_memory(void) {
    mallocFailIn = 0;
    MqttPayload prefixed = mqttMakeString("hello");
    TEST_ASSERT_TRUE(mqttPayloadIsFailure(prefixed));
    TEST_ASSERT_EQUAL_UINT32(PayloadFailOom, mqttPayloadReason(prefixed));
    TEST_ASSERT_EQUAL_UINT32(0, mqttPayloadLength(prefixed));

    mallocFailIn = 0;
    MqttPayload body = makeStringPayload("21.5");
    TEST_ASSERT_TRUE(mqttPayloadIsFailure(body));
    TEST_ASSERT_EQUAL_UINT32(PayloadFailOom, mqttPayloadReason(body));

    /* Not reason 4, which is what a stale {NULL, strlen("21.5")} would have
       decoded to under a scheme that counted reasons up from 1. */
    TEST_ASSERT_NOT_EQUAL_UINT32(4, mqttPayloadReason(body));
}

/* The builders that allocate a payload buffer of their own before framing
   it. The failure has to name the allocation, not the argument check it
   would otherwise trip on its way through buildMessage(). */
void test_builders_report_a_failed_payload_allocation(void) {
    Subscription subs[1] = {{.topic = "a/b", .qos = AtMostOnce}};
    const char* topics[] = {"a/b"};

    mallocFailIn = 0;
    MqttPayload connect = buildConnect("cid", NULL, NULL, NULL, NULL, false, 90);
    TEST_ASSERT_TRUE(mqttPayloadIsFailure(connect));
    TEST_ASSERT_EQUAL_UINT32(PayloadFailOom, mqttPayloadReason(connect));

    mallocFailIn = 0;
    MqttPayload subscribe = buildSubscribe(1, subs, 1);
    TEST_ASSERT_TRUE(mqttPayloadIsFailure(subscribe));
    TEST_ASSERT_EQUAL_UINT32(PayloadFailOom, mqttPayloadReason(subscribe));

    mallocFailIn = 0;
    MqttPayload unsubscribe = buildUnsubscribe(1, topics, 1);
    TEST_ASSERT_TRUE(mqttPayloadIsFailure(unsubscribe));
    TEST_ASSERT_EQUAL_UINT32(PayloadFailOom, mqttPayloadReason(unsubscribe));
}

/* Nothing is left behind on the way out of a failed build, whichever
   allocation is the one that fails. Under LSan the leak is the assertion;
   the reason check is what holds without it. */
void test_a_failed_build_leaks_nothing(void) {
    for (int failAt = 0; failAt < 3; failAt++) {
        mallocFailIn = failAt;
        MqttPayload p = buildPublishAck(1);
        if (mqttPayloadIsOk(p)) {
            mqttPayloadFree(&p);
            continue;
        }

        TEST_ASSERT_EQUAL_UINT32(PayloadFailOom, mqttPayloadReason(p));
    }
}

/* An empty body is a real body — it is how a retained topic gets cleared —
   so it comes back Ok, never a failure and never None. */
void test_empty_string_payload_is_ok_and_empty(void) {
    MqttPayload p = makeStringPayload("");
    TEST_ASSERT_TRUE(mqttPayloadIsOk(p));
    TEST_ASSERT_TRUE(mqttPayloadIsEmpty(p));
    TEST_ASSERT_FALSE(mqttPayloadIsFailure(p));
    TEST_ASSERT_FALSE(mqttPayloadIsNone(p));
    mqttPayloadFree(&p);
}

/* ── Bodies that are absent rather than empty ─────────────────────────── */

void test_bodyless_frames_parse_to_none(void) {
    MqttPayload frames[] = {
        buildPublishAck(1),
        buildPingReq(),
        buildDisconnectMsg(),
    };

    for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        FixedHeader fh = parseFixedHeader(frames[i]);
        Message m = parseMessage(fh, frames[i]);
        TEST_ASSERT_TRUE(mqttPayloadIsNone(m.payload));
        TEST_ASSERT_FALSE(mqttPayloadIsEmpty(m.payload));
        TEST_ASSERT_FALSE(mqttPayloadIsFailure(m.payload));
        mqttPayloadFree(&frames[i]);
    }
}

/* ── Every entry point refuses a failure ──────────────────────────────── */

/* Not hardening. The library now manufactures {NULL, tag} deliberately, so
   one can be handed back in through any of these. */

void test_parseFixedHeader_refuses_a_failure(void) {
    /* Before the entry-point check this walked into buffer[0]: the tag is
       non-zero, so the "did a byte arrive" test passed. */
    FixedHeader fh = parseFixedHeader(mqttPayloadFailure(PayloadFailOom));
    TEST_ASSERT_EQUAL_INT(FixedHeaderInvalid, fh.status);
}

void test_parseFixedHeader_treats_none_as_incomplete(void) {
    FixedHeader fh = parseFixedHeader(mqttPayloadNone());
    TEST_ASSERT_EQUAL_INT(FixedHeaderIncomplete, fh.status);
}

void test_parseMessage_refuses_a_failure(void) {
    FixedHeader fh;
    memset(&fh, 0, sizeof(fh));
    fh.type = ConnAck; /* the branch that reads at a fixed offset */
    fh.lenBytes = 1;
    fh.status = FixedHeaderOk;

    Message m = parseMessage(fh, mqttPayloadFailure(PayloadFailBadArgs));
    TEST_ASSERT_EQUAL_INT(MessageMalformed, m.status);
    TEST_ASSERT_TRUE(mqttPayloadIsNone(m.payload));
}

void test_buildPublish_refuses_a_failure(void) {
    MqttPayload p = buildPublish("t", 1, mqttPayloadFailure(PayloadFailOom), false);
    TEST_ASSERT_TRUE(mqttPayloadIsFailure(p));
    TEST_ASSERT_EQUAL_UINT32(PayloadFailBadArgs, mqttPayloadReason(p));
    mqttPayloadFree(&p);
}

/* The failure reason is a number a builder could mistake for a length and
   add to: 0xFFFF0001 plus a ten-byte header lands back inside the reserved
   range and decodes as a different reason — a wrong answer rather than a
   refusal. */
void test_buildPublish_does_not_carry_a_tag_into_the_result(void) {
    MqttPayload p = buildPublish("t", 1, mqttPayloadFailure(PayloadFailOom), false);
    TEST_ASSERT_NOT_EQUAL_UINT32(0xFFFF000Bu, mqttPayloadReason(p));
    TEST_ASSERT_EQUAL_UINT32(PayloadFailBadArgs, mqttPayloadReason(p));
    mqttPayloadFree(&p);
}

void test_buildPublish_accepts_a_none_body(void) {
    MqttPayload p = buildPublish("t", 1, mqttPayloadNone(), false);
    TEST_ASSERT_TRUE(mqttPayloadIsOk(p));
    mqttPayloadFree(&p);
}

/* ── Prefixes ─────────────────────────────────────────────────────────── */

void test_prefix_narrows_an_ok_payload(void) {
    uint8_t bytes[] = {1, 2, 3, 4};
    MqttPayload p = mqttPayloadPrefix(mqttPayloadFromBytes(bytes, sizeof(bytes)), 2);
    TEST_ASSERT_TRUE(mqttPayloadIsOk(p));
    TEST_ASSERT_EQUAL_UINT32(2, mqttPayloadLength(p));
    TEST_ASSERT_EQUAL_UINT8(1, mqttPayloadBytes(p)[0]);

    /* Asking for more than there is keeps what there is. */
    MqttPayload all = mqttPayloadPrefix(mqttPayloadFromBytes(bytes, sizeof(bytes)), 99);
    TEST_ASSERT_EQUAL_UINT32(4, mqttPayloadLength(all));
}

void test_prefix_of_a_failure_is_the_same_failure(void) {
    MqttPayload p = mqttPayloadPrefix(mqttPayloadFailure(PayloadFailOverflow), 2);
    TEST_ASSERT_TRUE(mqttPayloadIsFailure(p));
    TEST_ASSERT_EQUAL_UINT32(PayloadFailOverflow, mqttPayloadReason(p));

    MqttPayload none = mqttPayloadPrefix(mqttPayloadNone(), 2);
    TEST_ASSERT_TRUE(mqttPayloadIsNone(none));
}

/* ── Copying a body out ───────────────────────────────────────────────── */

void test_toCStr_copies_and_terminates(void) {
    uint8_t bytes[] = {'o', 'n'};
    char dst[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
    TEST_ASSERT_EQUAL_UINT32(2,
                             mqttPayloadToCStr(dst, sizeof(dst), mqttPayloadFromBytes(bytes, 2)));
    TEST_ASSERT_EQUAL_STRING("on", dst);
}

void test_toCStr_truncates_and_reports_the_full_length(void) {
    uint8_t bytes[] = {'o', 'f', 'f', 'l', 'i', 'n', 'e'};
    char dst[4];
    TEST_ASSERT_EQUAL_UINT32(7,
                             mqttPayloadToCStr(dst, sizeof(dst), mqttPayloadFromBytes(bytes, 7)));
    TEST_ASSERT_EQUAL_STRING("off", dst);
}

void test_toCStr_measures_with_no_buffer(void) {
    uint8_t bytes[] = {'o', 'n'};
    TEST_ASSERT_EQUAL_UINT32(2, mqttPayloadToCStr(NULL, 0, mqttPayloadFromBytes(bytes, 2)));

    char dst[1] = {'x'};
    TEST_ASSERT_EQUAL_UINT32(2, mqttPayloadToCStr(dst, 0, mqttPayloadFromBytes(bytes, 2)));
    TEST_ASSERT_EQUAL_CHAR('x', dst[0]);
}

/* The reason this goes through the accessors: a failure's length is a tag,
   and copying it as one would read four billion bytes from NULL. */
void test_toCStr_of_a_failure_is_an_empty_string(void) {
    char dst[8] = {'x'};
    TEST_ASSERT_EQUAL_UINT32(
        0, mqttPayloadToCStr(dst, sizeof(dst), mqttPayloadFailure(PayloadFailOom)));
    TEST_ASSERT_EQUAL_STRING("", dst);

    TEST_ASSERT_EQUAL_UINT32(0, mqttPayloadToCStr(dst, sizeof(dst), mqttPayloadNone()));
    TEST_ASSERT_EQUAL_STRING("", dst);
}

void test_toCStr_of_an_empty_body_is_an_empty_string(void) {
    uint8_t bytes[] = {'x'};
    char dst[8] = {'y'};
    TEST_ASSERT_EQUAL_UINT32(0,
                             mqttPayloadToCStr(dst, sizeof(dst), mqttPayloadFromBytes(bytes, 0)));
    TEST_ASSERT_EQUAL_STRING("", dst);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_none_is_not_ok_not_empty_not_failure);
    RUN_TEST(test_failure_is_only_a_failure);
    RUN_TEST(test_ok_payload_carries_its_bytes);
    RUN_TEST(test_empty_is_present_with_no_bytes);

    RUN_TEST(test_failure_has_no_length_and_no_bytes);
    RUN_TEST(test_untagged_failure_is_not_decoded);
    RUN_TEST(test_failure_constructor_rejects_a_bogus_reason);

    RUN_TEST(test_free_is_safe_on_every_state);

    RUN_TEST(test_every_builder_is_ok_on_success);
    RUN_TEST(test_builder_reports_out_of_memory_as_a_failure);
    RUN_TEST(test_builder_reports_a_failed_frame_allocation);
    RUN_TEST(test_string_constructors_report_out_of_memory);
    RUN_TEST(test_builders_report_a_failed_payload_allocation);
    RUN_TEST(test_a_failed_build_leaks_nothing);
    RUN_TEST(test_empty_string_payload_is_ok_and_empty);

    RUN_TEST(test_bodyless_frames_parse_to_none);

    RUN_TEST(test_parseFixedHeader_refuses_a_failure);
    RUN_TEST(test_parseFixedHeader_treats_none_as_incomplete);
    RUN_TEST(test_parseMessage_refuses_a_failure);
    RUN_TEST(test_buildPublish_refuses_a_failure);
    RUN_TEST(test_buildPublish_does_not_carry_a_tag_into_the_result);
    RUN_TEST(test_buildPublish_accepts_a_none_body);

    RUN_TEST(test_prefix_narrows_an_ok_payload);
    RUN_TEST(test_prefix_of_a_failure_is_the_same_failure);

    RUN_TEST(test_toCStr_copies_and_terminates);
    RUN_TEST(test_toCStr_truncates_and_reports_the_full_length);
    RUN_TEST(test_toCStr_measures_with_no_buffer);
    RUN_TEST(test_toCStr_of_a_failure_is_an_empty_string);
    RUN_TEST(test_toCStr_of_an_empty_body_is_an_empty_string);

    return UNITY_END();
}
