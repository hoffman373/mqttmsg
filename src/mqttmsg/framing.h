/**
 * @file framing.h
 * @internal
 * @brief The receive-side framing loop, kept apart from the transports so
 *        it can be tested off-target.
 *
 * TCP hands over a byte stream, not messages. What arrives in one read may
 * be half a frame, three frames, or two frames and the first byte of a
 * fourth. This is the state machine that turns that back into whole
 * messages: accumulate, parse the fixed header, dispatch anything complete,
 * shuffle the remainder to the front and go again.
 *
 * Deliberately free of both lwIP and the client. Nothing here knows what a
 * socket is, and the buffer belongs to the caller — for the client that is
 * its receive buffer, on a Pico filled straight from a pbuf; in a test it
 * is an array.
 *
 * Internal to the library; not shipped in `include/`.
 */

#ifndef MQTTMSG_FRAMING_H
#define MQTTMSG_FRAMING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include <mqttmsg/mqtt_types.h>

/**
 * @internal
 * @brief Called once per complete message.
 * @param ctx         The context pointer the framer was built with.
 * @param fixedHeader Header of the completed message.
 * @param payload     Body of the completed message.
 * @warning Both @p fixedHeader and @p payload are valid only for the
 *          duration of the call — the next iteration moves the bytes they
 *          point at.
 */
typedef void (*FramedMessageHandler)(void* ctx, FixedHeader* fixedHeader, MqttPayload* payload);

/** @internal @brief Outcome of one framing pass. */
typedef enum {
    /**
     * Everything complete was dispatched. Any remainder is a partial frame
     * still waiting on bytes, and is kept at the front of the buffer.
     */
    FramingOk = 0,
    /** More bytes offered than the buffer can hold. Nothing was copied. */
    FramingOverflow = 1,
    /**
     * A remaining-length field that never terminated inside the protocol's
     * four-byte limit: the stream is desynced rather than merely short, and
     * the connection cannot be recovered by waiting. The buffer is emptied.
     */
    FramingMalformed = 2
} FramingStatus;

/** @internal @brief The framing state machine and the buffer it works over. */
typedef struct {
    uint8_t* buffer; /**< Caller-owned accumulation buffer. */
    int capacity;    /**< Bytes @c buffer can hold. */
    /**
     * Bytes currently held. Survives across feeds — a partial frame lives
     * here until the rest of it arrives.
     */
    int length;
    FramedMessageHandler onMessage; /**< Called for each complete message. */
    void* ctx;                      /**< Passed back to @c onMessage. */
} MessageFramer;

/**
 * @internal
 * @brief Room left for incoming bytes.
 * @param framer Framer to query.
 * @return Free bytes in the buffer.
 * @note Callers that write into the buffer themselves — the Pico path
 *       copies out of a pbuf directly, to avoid staging 2 KB twice — must
 *       check this first and treat a shortfall as ::FramingOverflow.
 */
static inline int framerRemaining(const MessageFramer* framer) {
    return framer->capacity - framer->length;
}

/**
 * @internal
 * @brief Runs the loop over bytes the caller already wrote into the buffer.
 * @param framer     Framer to advance.
 * @param addedBytes How many bytes were written at `buffer + length`.
 * @return ::FramingOk, or ::FramingMalformed if the stream desynced.
 */
FramingStatus framerConsume(MessageFramer* framer, int addedBytes);

/**
 * @internal
 * @brief Copies bytes into the buffer, then runs the loop.
 *
 * For callers holding a contiguous buffer rather than writing in place.
 *
 * @param framer Framer to advance.
 * @param data   Bytes to copy in.
 * @param length How many bytes to copy.
 * @return ::FramingOk, ::FramingOverflow if @p data would not fit — in
 *         which case nothing is copied — or ::FramingMalformed.
 */
FramingStatus framerFeed(MessageFramer* framer, const uint8_t* data, int length);

#ifdef __cplusplus
}
#endif

#endif
