/**
 * @file payload_writer.h
 * @internal
 * @brief A bounds-checked cursor for filling a buffer with an MQTT frame.
 *
 * Internal to the library. It lives under `src/` rather than `include/` on
 * purpose: consumers only get `include/` on their path, so this is not part
 * of the public API and can change freely. It is a header rather than
 * static functions inside `mqtt.c` only so the overflow behaviour can be
 * tested directly.
 */

#ifndef MQTTMSG_PAYLOAD_WRITER_H
#define MQTTMSG_PAYLOAD_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <mqttmsg/mqtt.h>
#include <mqttmsg/mqtt_string.h>

/**
 * @internal
 * @brief A cursor over a buffer being filled with an MQTT frame.
 *
 * A writer either writes or measures, depending on whether it has a buffer.
 * writerOver() writes into one; writerMeasure() has none and discards the
 * bytes, but still advances #writePos, so afterwards #writePos is the size
 * the frame would occupy. That gives an encoder its own sizing function:
 * run it against a measuring writer to learn how much to allocate, then
 * against a real one to fill the allocation.
 *
 * @code
 * PayloadWriter measure = writerMeasure();
 * writeFoo(&measure, ...);
 * uint8_t* buffer = malloc(measure.writePos);
 * PayloadWriter w = writerOver(buffer, measure.writePos);
 * writeFoo(&w, ...);
 * @endcode
 *
 * @warning A writer never writes outside its buffer. Anything that would
 *          exceed #capacity sets #overflowed instead, and an overflowed
 *          writer stops accepting writes entirely rather than filling the
 *          space that was left — so a frame is either complete or visibly
 *          failed, never truncated. Check #overflowed once at the end;
 *          individual writes do not report.
 */
typedef struct {
    uint8_t* buffer; /**< Destination, or NULL when measuring. */
    size_t capacity; /**< Bytes @c buffer can hold. */
    size_t writePos; /**< Bytes written so far, or measured. */
    bool overflowed; /**< Set once a write would have exceeded @c capacity. */
    /**
     * Whether this writer measures rather than writes.
     *
     * Held rather than inferred from a NULL #buffer, because the two are not
     * the same thing: a writer over an allocation that failed also has no
     * buffer, and treating that as a measuring pass is how a frame gets
     * silently discarded and reported as written. See writerOver().
     */
    bool measuring;
} PayloadWriter;

/**
 * @internal
 * @brief Creates a writer that measures instead of writing.
 * @return A writer with no buffer. After running an encoder against it,
 *         PayloadWriter::writePos is the size the frame needs.
 */
static inline PayloadWriter writerMeasure(void) {
    PayloadWriter w = {NULL, 0, 0, false, true};
    return w;
}

/**
 * @internal
 * @brief Creates a writer over a buffer.
 * @param buffer   Destination. NULL — an allocation that failed — yields a
 *                 writer that is already overflowed, so the caller sees the
 *                 failure at the end rather than a frame that was quietly
 *                 thrown away.
 * @param capacity Bytes @p buffer can hold.
 * @return A writer positioned at the start of @p buffer.
 */
static inline PayloadWriter writerOver(uint8_t* buffer, size_t capacity) {
    PayloadWriter w = {buffer, capacity, 0, buffer == NULL, false};
    return w;
}

/**
 * @internal
 * @brief Reserves space and returns where to put it.
 * @param w   Writer to advance.
 * @param len Bytes to reserve.
 * @return Where to write, or NULL if there is nowhere to put them — either
 *         the writer is measuring or it has overflowed. Which of those it is
 *         is the difference between a frame that was sized and one that was
 *         lost, so callers read PayloadWriter::overflowed rather than this.
 * @note PayloadWriter::writePos advances regardless, so a measuring writer
 *       still accumulates the size.
 */
static inline uint8_t* writerTake(PayloadWriter* w, size_t len) {
    if (w->overflowed) {
        return NULL;
    }

    if (!w->measuring && w->writePos + len > w->capacity) {
        w->overflowed = true;
        return NULL;
    }

    size_t at = w->writePos;
    w->writePos += len;
    return w->measuring ? NULL : w->buffer + at;
}

/**
 * @internal
 * @brief Writes one byte.
 * @param w     Writer to advance.
 * @param value Byte to write.
 */
static inline void writeByte(PayloadWriter* w, uint8_t value) {
    uint8_t* at = writerTake(w, 1);
    if (at != NULL) {
        *at = value;
    }
}

/**
 * @internal
 * @brief Writes a 16-bit value in MQTT's big-endian byte order.
 * @param w     Writer to advance.
 * @param value Value to write.
 */
static inline void writeShort(PayloadWriter* w, uint16_t value) {
    uint8_t* at = writerTake(w, 2);
    if (at != NULL) {
        mqttShortToBytes(at, value);
    }
}

/**
 * @internal
 * @brief Writes a run of raw bytes.
 * @param w   Writer to advance.
 * @param src Bytes to copy.
 * @param len How many bytes to copy.
 */
static inline void writeBytes(PayloadWriter* w, const uint8_t* src, size_t len) {
    uint8_t* at = writerTake(w, len);
    if (at != NULL && len > 0) {
        memcpy(at, src, len);
    }
}

/**
 * @internal
 * @brief Writes a length-prefixed string, the wire encoding for every MQTT
 *        string field.
 * @param w         Writer to advance.
 * @param toConvert String to write.
 */
static inline void putView(PayloadWriter* w, MqttString toConvert) {
    uint8_t* at = writerTake(w, (size_t)toConvert.length + 2);
    if (at != NULL) {
        mqttShortToBytes(at, toConvert.length);
        memcpy(at + 2, toConvert.data, toConvert.length);
    }
}

/**
 * @internal
 * @brief Writes a NUL-terminated C string, length-prefixed.
 * @param w            Writer to advance.
 * @param strToConvert String to write.
 * @note Routed through putView(); mqttStrFromCStr() folds to a constant for
 *       string literals, so this costs nothing over a dedicated path.
 */
static inline void putString(PayloadWriter* w, const char* strToConvert) {
    putView(w, mqttStrFromCStr(strToConvert));
}

#endif
