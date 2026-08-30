/* Tests for mqttCheckKey() (src/mqttmsg/message_auth.c).

   The file had no coverage at all until now, which is part of why an
   unchecked allocation sat in it. The expected hashes below were generated
   with Python's hmac and hashlib — HMAC-SHA256(key, SHA256(message) ||
   nonce) — rather than captured from this implementation, so they pin the
   scheme itself and would catch a refactor that changed the bytes being
   hashed. They are also the independent check on the HMAC here: a padding
   or block-size mistake in it would not agree with a reference. */

#include "unity/unity.h"
#include <mqttmsg/keymanager.h>
#include <mqttmsg/mqtt.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const char* const KEY = "s3cr3t";
static const uint8_t NONCE[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
#define NONCE_LEN ((size_t)sizeof(NONCE))
static const char* const MESSAGE = "relay/1 on";

/* HMAC-SHA256("s3cr3t", SHA256("relay/1 on") || "0123456789abcdef") */
static uint8_t expected[32] = {
    0xab, 0x59, 0xf9, 0xa3, 0x95, 0x7b, 0x6b, 0xc2, 0x5c, 0xaf, 0xc2, 0x63, 0xdd, 0x64, 0xeb, 0x94,
    0x42, 0xa5, 0x62, 0x90, 0x12, 0x7f, 0x9f, 0x6f, 0x51, 0xd4, 0x02, 0xeb, 0xec, 0x1d, 0xc4, 0x11,
};

/* Same key and nonce, empty message. */
static uint8_t expectedEmptyMessage[32] = {
    0x26, 0xee, 0x9d, 0x8a, 0x52, 0x83, 0xa8, 0xc0, 0x64, 0x0b, 0x6f, 0xef, 0x13, 0x24, 0xfe, 0x47,
    0x39, 0x9a, 0x6f, 0xd8, 0x04, 0x42, 0xd7, 0x16, 0xd7, 0x3b, 0x18, 0x02, 0x84, 0x69, 0x2b, 0x37,
};

/* Same message, empty key and nonce. */
static uint8_t expectedEmptyKeyAndNonce[32] = {
    0xd1, 0x97, 0x77, 0x3f, 0x95, 0x80, 0xbf, 0x8f, 0xde, 0x80, 0x34, 0x26, 0x86, 0xd8, 0x43, 0x26,
    0x7d, 0x25, 0xbf, 0x5a, 0x29, 0x16, 0xb9, 0x5e, 0x7d, 0x88, 0xa9, 0x68, 0x53, 0x3d, 0x1e, 0x3b,
};

/* A key one byte longer than HMAC's 64-byte block, which HMAC replaces with
   its own digest before padding. Written as 65 'k's, matching the Python
   that produced the vector below. */
static const char* const LONG_KEY =
    "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk";

/* HMAC-SHA256(65 * "k", SHA256("relay/1 on") || "0123456789abcdef") */
static uint8_t expectedLongKey[32] = {
    0x28, 0x9c, 0xb0, 0x77, 0x04, 0x80, 0x91, 0x28, 0xb5, 0x9a, 0x1a, 0xc4, 0xb1, 0x73, 0xe6, 0x6e,
    0x55, 0x84, 0xdf, 0x7e, 0xb4, 0x00, 0xa8, 0xf6, 0xef, 0x68, 0xef, 0x1a, 0x8d, 0x8f, 0x17, 0x93,
};

/* A nonce with a zero byte in the middle. makeNonce() fills 32 uniformly
   random bytes, so about one nonce in eight contains one of these; the
   digest below covers all five bytes. */
static const uint8_t NONCE_WITH_ZERO[] = {'a', 'b', 0x00, 'c', 'd'};

/* HMAC-SHA256("s3cr3t", SHA256("relay/1 on") || "ab\0cd") -- all 5 bytes. */
static uint8_t expectedNonceWithZero[32] = {
    0x21, 0x53, 0xd6, 0x91, 0xc2, 0x10, 0xa5, 0xb8, 0x43, 0xec, 0xb4, 0xad, 0x9a, 0x0a, 0xac, 0x72,
    0x7f, 0x97, 0x61, 0x13, 0x81, 0xf8, 0x1e, 0x41, 0x3a, 0x67, 0x92, 0xde, 0x64, 0x88, 0x66, 0x66,
};

/* The same call as strlen() used to see it: only "ab", the two bytes before
   the zero. This is what the old signature silently authenticated. */
static uint8_t expectedNonceTruncatedAtZero[32] = {
    0xeb, 0xbd, 0x12, 0xb1, 0x6b, 0x5b, 0xb5, 0xed, 0x64, 0xe8, 0x94, 0xce, 0xd8, 0xef, 0x06, 0xcc,
    0x59, 0x3b, 0x6d, 0x4d, 0x1f, 0xa1, 0x6c, 0x4f, 0x34, 0x5d, 0x4a, 0xb7, 0x19, 0xc3, 0xdf, 0xf8,
};

static MqttPayload messagePayload(const char* text) {
    return mqttPayloadFromBytes((uint8_t*)text, (uint32_t)strlen(text));
}

/* ── The scheme ───────────────────────────────────────────────────── */

void test_matching_hash_verifies(void) {
    TEST_ASSERT_TRUE(mqttCheckKey(messagePayload(MESSAGE), KEY, NONCE, NONCE_LEN, expected));
}

void test_empty_message_verifies(void) {
    /* A body that is present and carries no bytes still hashes, and to a
       different digest than a body with content. */
    MqttPayload empty = mqttPayloadFromBytes((uint8_t*)"", 0);
    TEST_ASSERT_TRUE(mqttPayloadIsEmpty(empty));
    TEST_ASSERT_TRUE(mqttCheckKey(empty, KEY, NONCE, NONCE_LEN, expectedEmptyMessage));
}

void test_empty_key_and_nonce_verify(void) {
    /* Empty is not the same as absent: these hash to a real digest, where a
       NULL is refused outright. An empty key is a zero-filled HMAC block
       like any other key shorter than the block. */
    TEST_ASSERT_TRUE(
        mqttCheckKey(messagePayload(MESSAGE), "", (const uint8_t*)"", 0, expectedEmptyKeyAndNonce));
}

void test_key_longer_than_the_hmac_block_verifies(void) {
    /* The one branch in the HMAC that a short key never reaches: over the
       block length, the key is hashed down to 32 bytes first. Getting that
       wrong -- truncating instead, or padding past the block -- still
       produces a stable digest, so only a reference vector catches it. */
    TEST_ASSERT_EQUAL_UINT(65, strlen(LONG_KEY));
    TEST_ASSERT_TRUE(
        mqttCheckKey(messagePayload(MESSAGE), LONG_KEY, NONCE, NONCE_LEN, expectedLongKey));
}

void test_nonce_is_counted_not_measured(void) {
    /* The regression the counted length exists for. A nonce carrying a zero
       byte must hash in full; measuring it with strlen() would stop at the
       zero and authenticate a shorter nonce than the sender used, matching
       on both sides and so failing silently. */
    TEST_ASSERT_TRUE(mqttCheckKey(messagePayload(MESSAGE), KEY, NONCE_WITH_ZERO,
                                  sizeof(NONCE_WITH_ZERO), expectedNonceWithZero));

    /* The other half: the truncated digest must not verify against the full
       nonce, or the test above would pass even if the bytes past the zero
       were being dropped. */
    TEST_ASSERT_FALSE(mqttCheckKey(messagePayload(MESSAGE), KEY, NONCE_WITH_ZERO,
                                   sizeof(NONCE_WITH_ZERO), expectedNonceTruncatedAtZero));
}

/* ── What must not verify ─────────────────────────────────────────── */

void test_wrong_key_is_rejected(void) {
    TEST_ASSERT_FALSE(mqttCheckKey(messagePayload(MESSAGE), "wrong", NONCE, NONCE_LEN, expected));
}

/* The point of the nonce: a recorded message replayed under a later one no
   longer verifies. */
void test_wrong_nonce_is_rejected(void) {
    TEST_ASSERT_FALSE(mqttCheckKey(messagePayload(MESSAGE), KEY, (const uint8_t*)"fedcba9876543210",
                                   16, expected));
}

void test_tampered_message_is_rejected(void) {
    TEST_ASSERT_FALSE(mqttCheckKey(messagePayload("relay/1 off"), KEY, NONCE, NONCE_LEN, expected));
}

void test_one_flipped_bit_in_the_hash_is_rejected(void) {
    uint8_t altered[32];
    memcpy(altered, expected, sizeof(altered));
    altered[31] ^= 0x01;
    TEST_ASSERT_FALSE(mqttCheckKey(messagePayload(MESSAGE), KEY, NONCE, NONCE_LEN, altered));
}

void test_a_hash_wrong_in_its_first_byte_is_rejected(void) {
    /* The pair with the test above, at the other end of the hash. Both must
       be rejected, and the comparison reads all 32 bytes either way — a
       rejection that returned as soon as it found the wrong byte would take
       measurably less time here than there, which is how a forged hash gets
       guessed a byte at a time. Timing is not something a unit test can
       assert; what this pins is that neither end is treated specially. */
    uint8_t altered[32];
    memcpy(altered, expected, sizeof(altered));
    altered[0] ^= 0x01;
    TEST_ASSERT_FALSE(mqttCheckKey(messagePayload(MESSAGE), KEY, NONCE, NONCE_LEN, altered));
}

/* ── Arguments that are not there ─────────────────────────────────── */

/* Every one of these must come back false, not fault — the unguarded path
   is a strlen() of NULL, or a hash read from one. */
void test_null_arguments_are_rejected(void) {
    MqttPayload message = messagePayload(MESSAGE);
    TEST_ASSERT_FALSE(mqttCheckKey(message, NULL, NONCE, NONCE_LEN, expected));
    TEST_ASSERT_FALSE(mqttCheckKey(message, KEY, NULL, NONCE_LEN, expected));
    TEST_ASSERT_FALSE(mqttCheckKey(message, KEY, NONCE, NONCE_LEN, NULL));
}

void test_unusable_message_is_rejected(void) {
    /* A frame that could not be built authenticates nothing, and its failure
       tag must not be read as a length by the hash. */
    TEST_ASSERT_FALSE(
        mqttCheckKey(mqttPayloadFailure(PayloadFailOom), KEY, NONCE, NONCE_LEN, expected));
    TEST_ASSERT_FALSE(mqttCheckKey(mqttPayloadNone(), KEY, NONCE, NONCE_LEN, expected));
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_matching_hash_verifies);
    RUN_TEST(test_empty_message_verifies);
    RUN_TEST(test_empty_key_and_nonce_verify);
    RUN_TEST(test_key_longer_than_the_hmac_block_verifies);
    RUN_TEST(test_nonce_is_counted_not_measured);

    RUN_TEST(test_wrong_key_is_rejected);
    RUN_TEST(test_wrong_nonce_is_rejected);
    RUN_TEST(test_tampered_message_is_rejected);
    RUN_TEST(test_one_flipped_bit_in_the_hash_is_rejected);
    RUN_TEST(test_a_hash_wrong_in_its_first_byte_is_rejected);

    RUN_TEST(test_null_arguments_are_rejected);
    RUN_TEST(test_unusable_message_is_rejected);

    return UNITY_END();
}
