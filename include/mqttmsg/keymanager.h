/**
 * @file keymanager.h
 * @brief Nonce generation and message authentication over HMAC-SHA256.
 *
 * Guards against a replayed command: the sender hashes the message, then
 * takes an HMAC over that digest and a nonce the receiver issued, keyed
 * with a shared secret. A recorded message replayed under a later nonce no
 * longer verifies.
 *
 * The message is pre-hashed so that what the key covers is a fixed 32
 * bytes and the nonce, whatever the body's length. HMAC rather than a
 * hand-rolled keyed hash because it is the construction with a proof
 * behind it, and costs the same here -- the same sha256_update() calls
 * over two extra 64-byte blocks.
 *
 * @warning The SHA-256 implementation this uses is Brad Conte's, vendored
 *          as a submodule; upstream is explicit that it has no resistance
 *          to side-channel attacks.
 * @note makeNonce() is device-only — it reads the RP2350 ring oscillator
 *       directly — and lives in keymanager.c. mqttCheckKey() is portable and
 *       lives in message_auth.c, where the host tests reach it.
 */

#ifndef MQTTMSG_KEYMANAGER_H
#define MQTTMSG_KEYMANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <mqttmsg/mqtt_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @def MQTTMSG_NONCE_BYTES
 * @brief Length of the nonce makeNonce() writes, in bytes.
 *
 * The size of the buffer to hand makeNonce(), and the length to pass
 * mqttCheckKey() alongside it, so the two halves cannot disagree.
 */
#define MQTTMSG_NONCE_BYTES 32

/**
 * @brief Verifies a message against its authentication hash.
 *
 * Computes `HMAC-SHA256(key, SHA256(message) || nonce)` and compares it
 * with @p inHash.
 *
 * @param message     Bytes that were hashed.
 * @param key         NUL-terminated shared secret.
 * @param nonce       Nonce the message was sent under, as raw bytes.
 * @param nonceLength How many bytes of @p nonce to hash.
 * @param inHash      The 32-byte hash received alongside the message.
 * @return true if the hashes match. Nothing verifies against a NULL
 *         argument or a @p message that is not mqttPayloadIsOk(); an empty
 *         @p key or @p nonce is a value like any other and does hash.
 * @note @p nonce is counted, not NUL-terminated: it is raw bytes and may
 *       contain a zero. Pass #MQTTMSG_NONCE_BYTES for a nonce makeNonce()
 *       filled. @p key stays NUL-terminated: it is a configured secret.
 * @note @p key is used at its full length: keys longer than HMAC's 64-byte
 *       block are hashed down rather than truncated, so two long keys
 *       sharing a prefix stay distinct.
 * @note Allocates nothing: the digest and nonce are fed to SHA-256 in
 *       sequence rather than concatenated into a buffer first.
 * @note The final comparison is constant-time: it always reads all 32 bytes,
 *       so how long a rejection takes says nothing about how much of @p
 *       inHash was right. The SHA-256 underneath makes no such promise; see
 *       the warning above.
 */
bool mqttCheckKey(MqttPayload message, const char* key, const uint8_t* nonce, size_t nonceLength,
                  uint8_t* inHash);

/**
 * @brief Fills a fresh nonce from the hardware ring oscillator.
 * @param dest Destination, which must have room for #MQTTMSG_NONCE_BYTES.
 * @note The bytes may include zeroes; the result is not a C string. Pass it
 *       to mqttCheckKey() with an explicit length.
 */
void makeNonce(uint8_t* dest);

#ifdef __cplusplus
}
#endif

#endif
