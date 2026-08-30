/**
 * @file mqtt_payload.h
 * @brief A length-counted byte buffer, and the three states one can be in.
 *
 * A MqttPayload is a whole frame, a message body, or the failure that stopped
 * one being built. Which of those it is cannot be read off the fields, and
 * is not meant to be: build one with the constructors here, ask about it
 * with the predicates, and read it through the accessors. The encoding
 * behind them is this library's to change.
 *
 * The three states, and how they are spelled on the wire of the struct:
 *
 * | `buffer`   | `length`                     | State                     |
 * |------------|------------------------------|---------------------------|
 * | non-NULL   | any                          | Ok — @c length bytes      |
 * | NULL       | 0                            | None — no body at all     |
 * | NULL       | >= #MQTTMSG_PAYLOAD_TAG_MIN  | Failure, reason in length |
 * | NULL       | anything else non-zero       | Failure, reason unknown   |
 *
 * `{NULL, non-zero}` is meaningless as a payload, so the top of the
 * `uint32_t` range is free to carry a failure reason: a real length is
 * capped by MQTT's remaining-length field at 268,435,455, and in practice
 * by the 2 KB receive buffer. Nothing legitimate reaches
 * #MQTTMSG_PAYLOAD_TAG_MIN. That buys as many failure reasons as are ever
 * needed at no size cost — a MqttPayload is still two words.
 *
 * The reserved range starts at the top rather than counting up from 1 so
 * that a small stale length — `{NULL, 4}` from an allocation that failed
 * after the length was set — cannot be mistaken for a reason code. It is
 * still a failure, just one with no reason recorded.
 *
 * @note Distinguishing "no body" from "empty body" is the same distinction
 *       mqttStrIsEmpty() and mqttStrIsPresent() draw for strings, and it
 *       matters for the same reason: a PUBACK carries no body, while a
 *       zero-length retained PUBLISH carries an empty one, and the two do
 *       not encode alike.
 * @warning A caller that reads `.buffer` or `.length` itself has stepped
 *          outside the contract, and will see the failure tag as a
 *          four-billion-byte length. mqttPayloadLength() and
 *          mqttPayloadBytes() exist precisely so that never happens.
 */

#ifndef MQTTMSG_MQTT_PAYLOAD_H
#define MQTTMSG_MQTT_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @def MQTTMSG_PAYLOAD_FN
 * @brief Linkage for the inline functions below.
 *
 * `constexpr` in C++ so that a C++ caller keeps the compile-time evaluation
 * mqtt_string.hpp relies on — its `bytes()` is constexpr and goes through
 * these, rather than reading the fields it is not supposed to know about.
 * Plain `static inline` in C, where the question does not arise.
 * mqttPayloadFree() and mqttPayloadToCStr() are excluded: one calls free(),
 * the other writes through the caller's pointer.
 */
#ifdef __cplusplus
#define MQTTMSG_PAYLOAD_FN constexpr
#else
#define MQTTMSG_PAYLOAD_FN static inline
#endif

/** @brief A length-counted byte buffer: a whole frame, or a message body. */
typedef struct {
    uint8_t* buffer; /**< Private. Go through the accessors. */
    uint32_t length; /**< Private. Go through the accessors. */
} MqttPayload;

/**
 * @def MQTTMSG_PAYLOAD_TAG_MIN
 * @brief First `length` value reserved for a failure reason.
 *
 * Above every length the protocol can express, so no real payload collides
 * with a tag.
 */
#define MQTTMSG_PAYLOAD_TAG_MIN 0xFFFF0000u

/**
 * @brief Why a MqttPayload could not be produced.
 *
 * The values are the reserved `length` tags themselves, not an index into
 * them, so a tag can be read back as a reason without a lookup.
 */
typedef enum {
    /** Not a failure at all: what mqttPayloadReason() answers for Ok and None. */
    PayloadNoFailure = 0u,
    /**
     * A failure whose `length` is not one of the tags below — a stale length
     * left behind by an allocation that did not happen. Reported as a failure
     * rather than decoded, because the value means nothing.
     */
    PayloadFailUnknown = MQTTMSG_PAYLOAD_TAG_MIN,
    PayloadFailOom = 0xFFFF0001u,      /**< Allocation failed. */
    PayloadFailOverflow = 0xFFFF0002u, /**< The frame writer ran out of room. */
    PayloadFailBadArgs = 0xFFFF0003u   /**< An argument was itself not usable. */
} PayloadReason;

/**
 * @brief A message type that has no body at all.
 * @return A MqttPayload that is neither Ok nor a failure.
 */
MQTTMSG_PAYLOAD_FN MqttPayload mqttPayloadNone(void) {
    MqttPayload returnValue = {NULL, 0};
    return returnValue;
}

/**
 * @brief A MqttPayload that could not be produced.
 * @param reason Why. A value outside the reserved range is recorded as
 *               ::PayloadFailUnknown, so the encoding stays closed.
 * @return A MqttPayload that is mqttPayloadIsFailure().
 */
MQTTMSG_PAYLOAD_FN MqttPayload mqttPayloadFailure(PayloadReason reason) {
    MqttPayload returnValue = {NULL, (uint32_t)reason};
    if ((uint32_t)reason < MQTTMSG_PAYLOAD_TAG_MIN) {
        returnValue.length = (uint32_t)PayloadFailUnknown;
    }
    return returnValue;
}

/**
 * @brief Wraps bytes the caller already holds.
 *
 * The way to hand an existing buffer — a receive buffer, a test fixture, a
 * slice of a frame — to something that takes a MqttPayload. Nothing is copied
 * and nothing is allocated, so @p bytes must outlive the result.
 *
 * @param bytes  First byte, or NULL for mqttPayloadNone().
 * @param length How many bytes @p bytes holds.
 * @return An Ok MqttPayload over the range.
 */
MQTTMSG_PAYLOAD_FN MqttPayload mqttPayloadFromBytes(uint8_t* bytes, uint32_t length) {
    MqttPayload returnValue = {bytes, length};
    if (bytes == NULL) {
        returnValue.length = 0;
    }
    return returnValue;
}

/**
 * @brief Whether the MqttPayload carries bytes.
 *
 * The guard to write before using one. Spelling it as a single predicate
 * rather than `!isFailure && !isNone` is the point: the compound form is
 * easy to get half right.
 *
 * @param p MqttPayload to test.
 * @return true if @p p has a buffer. A length of zero still counts.
 */
MQTTMSG_PAYLOAD_FN bool mqttPayloadIsOk(MqttPayload p) { return p.buffer != NULL; }

/**
 * @brief Whether this message type has no body.
 * @param p MqttPayload to test.
 * @return true for the absence of a body, as PUBACK, PINGREQ and DISCONNECT
 *         all have.
 */
MQTTMSG_PAYLOAD_FN bool mqttPayloadIsNone(MqttPayload p) {
    return p.buffer == NULL && p.length == 0;
}

/**
 * @brief Whether producing the MqttPayload failed.
 * @param p MqttPayload to test.
 * @return true for both a tagged failure and an untagged one; see
 *         ::PayloadFailUnknown.
 */
MQTTMSG_PAYLOAD_FN bool mqttPayloadIsFailure(MqttPayload p) {
    return p.buffer == NULL && p.length != 0;
}

/**
 * @brief Whether the body is present and carries no bytes.
 *
 * Deliberately false for a failure and for None, so it can be tested in any
 * order against the other predicates. An empty body is a real body: it is
 * how a retained topic gets cleared.
 *
 * @param p MqttPayload to test.
 * @return true only for an Ok MqttPayload of zero length.
 */
MQTTMSG_PAYLOAD_FN bool mqttPayloadIsEmpty(MqttPayload p) {
    return p.buffer != NULL && p.length == 0;
}

/**
 * @brief How many bytes the MqttPayload carries.
 * @param p MqttPayload to measure.
 * @return The length, or 0 unless @p p is Ok — a failure tag is never
 *         observable as a length.
 */
MQTTMSG_PAYLOAD_FN uint32_t mqttPayloadLength(MqttPayload p) {
    return mqttPayloadIsOk(p) ? p.length : 0;
}

/**
 * @brief The bytes the MqttPayload carries.
 * @param p MqttPayload to read.
 * @return The first byte, or NULL unless @p p is Ok.
 */
MQTTMSG_PAYLOAD_FN const uint8_t* mqttPayloadBytes(MqttPayload p) {
    return mqttPayloadIsOk(p) ? p.buffer : NULL;
}

/**
 * @brief The first @p length bytes of a MqttPayload.
 *
 * What a stream reader holds when only part of a frame has arrived, and how
 * a test says so without reaching for the fields. Borrows: the result points
 * into @p p and does not own anything, so only @p p is released.
 *
 * @param p      MqttPayload to narrow.
 * @param length How many bytes to keep. More than @p p holds keeps all of
 *               them.
 * @return The prefix. A @p p that is not Ok comes back unchanged, so a
 *         failure narrows to the same failure rather than to a short read.
 */
MQTTMSG_PAYLOAD_FN MqttPayload mqttPayloadPrefix(MqttPayload p, uint32_t length) {
    if (!mqttPayloadIsOk(p)) {
        return p;
    }

    MqttPayload returnValue = {p.buffer, length < p.length ? length : p.length};
    return returnValue;
}

/**
 * @brief Why @p p is a failure.
 * @param p MqttPayload to ask about.
 * @return The reason, ::PayloadFailUnknown for a failure carrying no valid
 *         tag, or ::PayloadNoFailure if @p p is not a failure at all.
 */
MQTTMSG_PAYLOAD_FN PayloadReason mqttPayloadReason(MqttPayload p) {
    if (!mqttPayloadIsFailure(p)) {
        return PayloadNoFailure;
    }

    if (p.length >= MQTTMSG_PAYLOAD_TAG_MIN) {
        return (PayloadReason)p.length;
    }

    return PayloadFailUnknown;
}

/**
 * @brief Copies a body out as a NUL-terminated C string.
 *
 * The mqttStrToCStr() sibling for a payload, and the supported way to get a
 * C string out of one: it goes through the accessors, so a MqttPayload that has
 * no body or that failed to build copies nothing instead of reading a
 * failure tag as a length. Behaves snprintf-style — never writes more than
 * @p dstSize bytes and always terminates when @p dstSize is non-zero.
 *
 * @param dst     Destination buffer, or NULL to measure only.
 * @param dstSize Size of @p dst in bytes, terminator included.
 * @param p       MqttPayload to copy.
 * @return The length it wanted to write. A value `>= dstSize` means the
 *         result was truncated.
 * @warning A body is bytes, not text. One containing a NUL comes back as a
 *          C string that stops there, while the return value still counts
 *          the whole body; use mqttPayloadBytes() where that matters.
 */
static inline uint32_t mqttPayloadToCStr(char* dst, size_t dstSize, MqttPayload p) {
    uint32_t length = mqttPayloadLength(p);
    if (dst == NULL || dstSize == 0) {
        return length;
    }

    size_t toCopy = length;
    if (toCopy > dstSize - 1) {
        toCopy = dstSize - 1;
    }

    const uint8_t* bytes = mqttPayloadBytes(p);
    for (size_t i = 0; i < toCopy; i++) {
        dst[i] = (char)bytes[i];
    }

    dst[toCopy] = '\0';
    return length;
}

/**
 * @brief Releases an owned MqttPayload and leaves it None.
 *
 * The supported way to release anything a `build*()` function returned.
 * Safe on all three states, and safe to call twice: the second call sees a
 * None and has nothing to do.
 *
 * @param p MqttPayload to release; cleared in place. NULL is ignored.
 * @warning Only for Payloads that own their bytes. A MqttPayload out of
 *          parseMessage() borrows the caller's buffer and must not be freed.
 */
static inline void mqttPayloadFree(MqttPayload* p) {
    if (p == NULL) {
        return;
    }

    free(p->buffer);
    p->buffer = NULL;
    p->length = 0;
}

#ifdef __cplusplus
}
#endif

#endif
