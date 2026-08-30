/**
 * @file mqtt_string.h
 * @brief Zero-copy string views over an MQTT receive buffer.
 *
 * MQTT strings arrive as a 2-byte big-endian length followed by the bytes,
 * with no NUL terminator. An MqttString points directly at those bytes
 * where they already sit, instead of copying them into a heap-allocated C
 * string.
 *
 * Treat MqttString's fields as private and go through the functions here;
 * reach for `.data` only when handing the bytes to something outside this
 * library.
 *
 * @warning An MqttString borrows. It is valid only as long as the buffer it
 *          was parsed from is, which for a received message means the
 *          duration of the callback. Copy the bytes out with
 *          mqttStrToCStr() if you need to keep them.
 * @note This header pulls in nothing but C standard headers, so it is safe
 *       to include anywhere, including inside an `extern "C"` block. C++
 *       callers wanting std::string_view interop and range-for support
 *       want mqtt_string.hpp, which is not.
 */

#ifndef MQTTMSG_MQTT_STRING_H
#define MQTTMSG_MQTT_STRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A borrowed view of a string in a receive buffer. */
typedef struct {
    /** Not NUL-terminated. Points into the buffer the message was parsed from. */
    const char* data;
    uint16_t length; /**< Length in bytes. */
} MqttString;

/**
 * @def MQTT_STR_FMT
 * @brief printf conversion for an MqttString, to pair with #MQTT_STR_ARG.
 * @warning `%s` on an MqttString reads past the end of the string.
 */
#define MQTT_STR_FMT "%.*s"
/**
 * @def MQTT_STR_ARG
 * @brief Expands an MqttString into the arguments #MQTT_STR_FMT expects.
 *
 * @code
 * printf("topic: " MQTT_STR_FMT "\n", MQTT_STR_ARG(topic));
 * @endcode
 */
#define MQTT_STR_ARG(s) (int)(s).length, (s).data

/**
 * @brief An empty string.
 * @return A view with no bytes.
 * @note Distinct from a NULL `.data` only by convention; both compare equal
 *       to `""` and to each other.
 */
static inline MqttString mqttStrEmpty(void) {
    MqttString returnValue = {NULL, 0};
    return returnValue;
}

/**
 * @brief Whether the view carries no bytes.
 * @param s View to test.
 * @return true if `s.length` is zero, whether or not the field was present.
 */
static inline bool mqttStrIsEmpty(MqttString s) { return s.length == 0; }

/**
 * @brief Whether the field is there at all.
 *
 * A different question from whether it is empty. A message type with no
 * topic name leaves the field absent; a topic name of `""` is a field that
 * is present and carries zero bytes. Both are mqttStrIsEmpty(), only the
 * second is mqttStrIsPresent(), and the wire encoding differs: an absent
 * field contributes nothing, a present empty one contributes a 0x0000
 * length prefix.
 *
 * @param s View to test.
 * @return true if the field was present in the frame.
 */
static inline bool mqttStrIsPresent(MqttString s) { return s.data != NULL; }

/**
 * @brief Wraps a NUL-terminated C string as a view, so it can be compared
 *        against a parsed one.
 * @param cstr Source string, or NULL for an empty view.
 * @return A view over @p cstr. Strings longer than `UINT16_MAX` are
 *         truncated, which matches the MQTT wire limit.
 * @warning Does not copy; @p cstr must outlive the result.
 */
static inline MqttString mqttStrFromCStr(const char* cstr) {
    MqttString returnValue = {NULL, 0};
    if (cstr == NULL) {
        return returnValue;
    }

    size_t length = strlen(cstr);
    if (length > UINT16_MAX) {
        length = UINT16_MAX;
    }

    returnValue.data = cstr;
    returnValue.length = (uint16_t)length;
    return returnValue;
}

/**
 * @brief Builds a view over an explicit range.
 * @param bytes  First byte, or NULL for an empty view.
 * @param length How many bytes to cover.
 * @return A view over the range.
 * @warning No validation beyond the length cap; callers parsing wire data
 *          should bounds-check before calling.
 */
static inline MqttString mqttStrFromBytes(const char* bytes, uint16_t length) {
    MqttString returnValue = {bytes, length};
    if (bytes == NULL) {
        returnValue.length = 0;
    }
    return returnValue;
}

/**
 * @brief Byte-for-byte comparison of two views.
 * @param a First view.
 * @param b Second view.
 * @return true if both have the same length and the same bytes.
 */
static inline bool mqttStrEq(MqttString a, MqttString b) {
    if (a.length != b.length) {
        return false;
    }

    for (uint16_t i = 0; i < a.length; i++) {
        if (a.data[i] != b.data[i]) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Compares a view against a NUL-terminated C string.
 * @param a View to compare.
 * @param b C string to compare against.
 * @return true if they hold the same bytes.
 */
static inline bool mqttStrEqCStr(MqttString a, const char* b) {
    return mqttStrEq(a, mqttStrFromCStr(b));
}

/**
 * @brief Prefix match.
 *
 * What makes a trailing-`#` wildcard subscription work: match the topic
 * against the subscription with the `#` removed.
 *
 * @param s      View to test.
 * @param prefix Prefix to look for.
 * @return true if @p s starts with @p prefix.
 */
static inline bool mqttStrStartsWith(MqttString s, MqttString prefix) {
    if (prefix.length > s.length) {
        return false;
    }

    for (uint16_t i = 0; i < prefix.length; i++) {
        if (s.data[i] != prefix.data[i]) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Prefix match against a NUL-terminated C string.
 * @param s      View to test.
 * @param prefix Prefix to look for.
 * @return true if @p s starts with @p prefix.
 */
static inline bool mqttStrStartsWithCStr(MqttString s, const char* prefix) {
    return mqttStrStartsWith(s, mqttStrFromCStr(prefix));
}

/**
 * @brief Strips a topic prefix, separator included.
 *
 * The whole-segment form of mqttStrStartsWith(): it consumes @p prefix and
 * the `/` that has to follow it, so `"atx"` does not match
 * `"atxfoo/1/command"` the way a bare prefix test would.
 *
 * @param s      Topic to strip.
 * @param prefix Prefix to remove, without a trailing `/`.
 * @param rest   Receives what is left, or NULL to use this as a predicate.
 * @return true if @p s is under @p prefix, with @p rest empty when @p s is
 *         exactly the prefix.
 * @note An empty @p prefix matches everything and strips nothing, which is
 *       what a caller whose configured prefix is unset wants.
 */
static inline bool mqttStrStripTopicPrefix(MqttString s, MqttString prefix, MqttString* rest) {
    if (prefix.length == 0) {
        if (rest != NULL) {
            *rest = s;
        }

        return true;
    }

    if (!mqttStrStartsWith(s, prefix)) {
        return false;
    }

    /* The separator is part of what gets consumed, except where the topic
       ended at the prefix and there is none. */
    uint16_t consumed = prefix.length;
    if (s.length > prefix.length) {
        if (s.data[prefix.length] != '/') {
            return false;
        }

        consumed++;
    }

    if (rest != NULL) {
        *rest = mqttStrFromBytes(s.data + consumed, (uint16_t)(s.length - consumed));
    }

    return true;
}

/**
 * @brief Strips a topic prefix given as a NUL-terminated C string.
 * @param s      Topic to strip.
 * @param prefix Prefix to remove, or NULL to strip nothing.
 * @param rest   Receives what is left, or NULL to use this as a predicate.
 * @return true if @p s is under @p prefix.
 */
static inline bool mqttStrStripTopicPrefixCStr(MqttString s, const char* prefix, MqttString* rest) {
    return mqttStrStripTopicPrefix(s, mqttStrFromCStr(prefix), rest);
}

/**
 * @brief Copies a view out as a NUL-terminated C string.
 *
 * The supported way to get a C string out of a parsed message. Behaves
 * snprintf-style rather than strcpy-style: never writes more than
 * @p dstSize bytes and always terminates when @p dstSize is non-zero.
 *
 * @param dst     Destination buffer, or NULL to measure only.
 * @param dstSize Size of @p dst in bytes, terminator included.
 * @param s       View to copy.
 * @return The length it wanted to write. A value `>= dstSize` means the
 *         result was truncated.
 */
static inline uint16_t mqttStrToCStr(char* dst, size_t dstSize, MqttString s) {
    if (dst == NULL || dstSize == 0) {
        return s.length;
    }

    size_t toCopy = s.length;
    if (toCopy > dstSize - 1) {
        toCopy = dstSize - 1;
    }

    for (size_t i = 0; i < toCopy; i++) {
        dst[i] = s.data[i];
    }

    dst[toCopy] = '\0';
    return s.length;
}

/**
 * @brief Cursor over the `/`-separated segments of a topic.
 *
 * @code
 * MqttStrCursor cursor = mqttStrSegments(topic);
 * MqttString segment;
 * while (mqttStrNextSegment(&cursor, &segment)) { ... }
 * @endcode
 *
 * @note Empty segments are yielded, so `"a//b"` produces `"a"`, `""`,
 *       `"b"`.
 * @see mqttmsg::segments() in mqtt_string.hpp for a C++ range wrapper.
 */
typedef struct {
    const char* cur; /**< Start of the next segment. */
    const char* end; /**< One past the last byte of the topic. */
    bool done;       /**< Set once the final segment has been yielded. */
} MqttStrCursor;

/**
 * @brief Starts a cursor over the segments of @p s.
 * @param s Topic to walk.
 * @return A fresh cursor. An empty topic has no segments at all, rather
 *         than one empty segment.
 */
static inline MqttStrCursor mqttStrSegments(MqttString s) {
    MqttStrCursor returnValue;
    returnValue.cur = s.data;
    returnValue.end = s.data != NULL ? s.data + s.length : NULL;
    /* An empty topic has no segments at all, rather than one empty segment. */
    returnValue.done = s.data == NULL || s.length == 0;
    return returnValue;
}

/**
 * @brief Advances a segment cursor.
 * @param cursor Cursor to advance; updated in place.
 * @param out    Receives the next segment.
 * @return false once the segments are exhausted.
 */
static inline bool mqttStrNextSegment(MqttStrCursor* cursor, MqttString* out) {
    if (cursor == NULL || out == NULL || cursor->done) {
        return false;
    }

    const char* start = cursor->cur;
    const char* scan = start;
    while (scan < cursor->end && *scan != '/') {
        scan++;
    }

    out->data = start;
    out->length = (uint16_t)(scan - start);

    if (scan < cursor->end) {
        /* Step over the separator; a trailing '/' still yields a final empty
           segment, which is why done is set here rather than by cur == end. */
        cursor->cur = scan + 1;
    } else {
        cursor->done = true;
    }

    return true;
}

/**
 * @brief Matches a topic against a pattern, capturing what the wildcards
 *        matched.
 *
 * The same `+` and `#` vocabulary a subscription filter speaks, with the
 * addition that both capture. It is anchored at both ends: a pattern that
 * does not account for every segment of the topic does not match, which is
 * what makes it usable for routing rather than only for filtering.
 *
 * @code
 * MqttString captures[1];
 * if (mqttTopicMatch(topic, "atx/+/command", captures, 1, NULL)) {
 *     // captures[0] is the index segment.
 * }
 * @endcode
 *
 * @param topic        Topic to match.
 * @param pattern      NUL-terminated filter. `+` matches exactly one
 *                     segment, `#` matches the rest of the topic and is
 *                     only valid as the final segment; anything else is a
 *                     literal compared byte for byte.
 * @param captures     Receives what each wildcard matched, in pattern
 *                     order. May be NULL to match without capturing.
 * @param maxCaptures  How many entries @p captures holds. Never exceeded;
 *                     a wildcard past the end still matches, it is just not
 *                     recorded.
 * @param captureCount Receives how many entries were written, or NULL.
 * @return true if @p pattern consumes the whole of @p topic. A `#` that is
 *         not last makes the pattern unmatchable.
 * @note `#` matches the remaining segments, of which there may be none:
 *       `"a/#"` matches `"a"`, capturing an empty string. An empty segment
 *       is a segment, so `"+"` matches the empty one in `"a//b"`.
 * @warning The captures borrow @p topic's bytes and expire with it, like
 *          any other MqttString view.
 */
bool mqttTopicMatch(MqttString topic, const char* pattern, MqttString* captures,
                    uint8_t maxCaptures, uint8_t* captureCount);

/**
 * @brief mqttTopicMatch() under a topic prefix that is not known until run
 *        time.
 *
 * Equivalent to mqttStrStripTopicPrefix() followed by mqttTopicMatch(), so
 * that a configured prefix never has to be printed into a pattern literal
 * first.
 *
 * @param topic        Topic to match.
 * @param prefix       Prefix @p topic must be under, or NULL for none.
 * @param pattern      Filter to match against what is left after the
 *                     prefix.
 * @param captures     Receives what each wildcard matched, or NULL.
 * @param maxCaptures  How many entries @p captures holds.
 * @param captureCount Receives how many entries were written, or NULL.
 * @return true if @p topic is under @p prefix and the rest matches
 *         @p pattern.
 */
bool mqttTopicMatchUnder(MqttString topic, const char* prefix, const char* pattern,
                         MqttString* captures, uint8_t maxCaptures, uint8_t* captureCount);

/**
 * @brief Parses a view as an unsigned decimal integer.
 *
 * Digits and nothing else: no sign, no leading or trailing space, no
 * partial parse of a segment that starts with digits. A topic segment is
 * not a scanf field, and a caller routing on `"2abc"` wants to be told it
 * is not an index rather than handed a 2.
 *
 * @param s   View to parse. Leading zeros are fine.
 * @param out Receives the value, and is written only on success.
 * @return true if @p s is a decimal integer that fits in a `uint32_t`.
 */
bool mqttStrToU32(MqttString s, uint32_t* out);

#ifdef __cplusplus
}
#endif

#endif
