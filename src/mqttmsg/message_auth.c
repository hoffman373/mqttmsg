/**
 * @file message_auth.c
 * @brief HMAC-SHA256 message authentication.
 *
 * The half of keymanager.h that is arithmetic over bytes, so it builds and
 * is tested anywhere; keymanager.c holds the other half, which reads the
 * RP2350 ring oscillator and so builds only for the device. Same split,
 * and same reason, as framing.c and backoff.c.
 *
 * @see keymanager.h
 */

#include <string.h>

#include <crypto-algorithms/sha256.h>
#include <mqttmsg/keymanager.h>

/* HMAC-SHA256 operates on 64-byte blocks. Note that the vendored header's
   SHA256_BLOCK_SIZE is 32 -- that is the digest size, not the block size --
   so the pad length is written out here rather than taken from it. */
#define HMAC_BLOCK 64

/**
 * Computes HMAC-SHA256(key, digest || nonce) into @p out.
 *
 * The message half is passed as its two pieces rather than concatenated,
 * for the same reason the digest below is: SHA-256 takes its input in
 * sequence, so there is no buffer to allocate and no allocation to fail on
 * a path a broker can reach.
 */
static void hmacSha256(const BYTE* key, size_t keyLength, const BYTE* digest, const BYTE* nonce,
                       size_t nonceLength, BYTE* out) {
    SHA256_CTX ctx;

    /* A key longer than the block is replaced by its own digest, and one
       shorter is zero-padded out to the block. Both give the 64 bytes the
       pads are XORed over. */
    BYTE block[HMAC_BLOCK];
    memset(block, 0, sizeof(block));
    if (keyLength > HMAC_BLOCK) {
        sha256_init(&ctx);
        sha256_update(&ctx, key, keyLength);
        sha256_final(&ctx, block);
    } else {
        memcpy(block, key, keyLength);
    }

    BYTE pad[HMAC_BLOCK];
    for (size_t i = 0; i < HMAC_BLOCK; i++) {
        pad[i] = (BYTE)(block[i] ^ 0x36);
    }

    /* inner = SHA256((key ^ ipad) || digest || nonce) */
    BYTE inner[32];
    sha256_init(&ctx);
    sha256_update(&ctx, pad, HMAC_BLOCK);
    sha256_update(&ctx, digest, 32);
    sha256_update(&ctx, nonce, nonceLength);
    sha256_final(&ctx, inner);

    for (size_t i = 0; i < HMAC_BLOCK; i++) {
        pad[i] = (BYTE)(block[i] ^ 0x5c);
    }

    /* out = SHA256((key ^ opad) || inner) */
    sha256_init(&ctx);
    sha256_update(&ctx, pad, HMAC_BLOCK);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

bool mqttCheckKey(MqttPayload message, const char* key, const uint8_t* nonce, size_t nonceLength,
                  uint8_t* inHash) {
    /* Nothing to hash against: a NULL argument, or a payload that was never
       built. */
    if (key == NULL || nonce == NULL || inHash == NULL || !mqttPayloadIsOk(message)) {
        return false;
    }

    /* The message is hashed down to a fixed 32 bytes first, so what the key
       covers is one digest and the nonce rather than a body of any length. */
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, mqttPayloadBytes(message), mqttPayloadLength(message));

    BYTE digest[32];
    sha256_final(&ctx, digest);

    BYTE hash[32];
    /* The nonce is counted: it is raw bytes and may contain a zero. The key
       is a configured C string, so it measures. */
    hmacSha256((const BYTE*)key, strlen(key), digest, (const BYTE*)nonce, nonceLength, hash);

    /* Byte-at-a-time OR of the differences rather than memcmp(), which stops
       at the first byte that differs. That early exit makes verification take
       longer the more of a forged hash is right, which an attacker who can
       submit repeated attempts and time them turns into a byte-at-a-time
       search instead of a 2^256 one.

       The accumulator is volatile so the comparison stays a fixed 32 rounds:
       without it a compiler is free to notice the result is already decided
       and break out early, putting the timing leak back. */
    volatile uint8_t difference = 0;
    for (int i = 0; i < 32; i++) {
        difference |= (uint8_t)(hash[i] ^ inHash[i]);
    }

    return difference == 0;
}
