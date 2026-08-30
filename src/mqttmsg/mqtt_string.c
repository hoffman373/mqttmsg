/**
 * @file mqtt_string.c
 * @brief The parts of mqtt_string.h that are too large to inline: topic
 *        pattern matching and integer parsing.
 *
 * Everything here walks the same MqttStrCursor the header exposes, so no
 * bytes are copied and nothing is allocated. The matcher runs the pattern
 * and the topic through one cursor each and steps them together.
 *
 * @see mqtt_string.h
 */

#include <mqttmsg/mqtt_string.h>

/**
 * @brief Records one wildcard capture, if there is room for it.
 * @param captures    Where to write, or NULL when the caller is not
 *                    capturing.
 * @param maxCaptures How many entries @p captures holds.
 * @param count       How many have been written; advanced when this one is.
 * @param value       What the wildcard matched.
 */
static void capture(MqttString* captures, uint8_t maxCaptures, uint8_t* count, MqttString value) {
    if (captures == NULL || *count >= maxCaptures) {
        return;
    }

    captures[*count] = value;
    (*count)++;
}

/**
 * @brief What is left of a topic from where its cursor stands.
 *
 * The `#` capture. A cursor that is done has already yielded its last
 * segment and its position no longer means anything, which is the case
 * where `#` matches nothing at all.
 *
 * @param cursor Cursor to read the remainder from.
 * @return The remaining bytes, separators included.
 */
static MqttString topicRemainder(const MqttStrCursor* cursor) {
    if (cursor->done) {
        return mqttStrEmpty();
    }

    return mqttStrFromBytes(cursor->cur, (uint16_t)(cursor->end - cursor->cur));
}

bool mqttTopicMatch(MqttString topic, const char* pattern, MqttString* captures,
                    uint8_t maxCaptures, uint8_t* captureCount) {
    uint8_t captured = 0;
    if (captureCount != NULL) {
        *captureCount = 0;
    }

    MqttStrCursor patternCursor = mqttStrSegments(mqttStrFromCStr(pattern));
    MqttStrCursor topicCursor = mqttStrSegments(topic);

    MqttString patternSegment;
    while (mqttStrNextSegment(&patternCursor, &patternSegment)) {
        if (mqttStrEqCStr(patternSegment, "#")) {
            /* done is set by the segment that was just yielded, so anything
               after a '#' leaves it clear. */
            if (!patternCursor.done) {
                return false;
            }

            capture(captures, maxCaptures, &captured, topicRemainder(&topicCursor));
            if (captureCount != NULL) {
                *captureCount = captured;
            }

            return true;
        }

        MqttString topicSegment;
        if (!mqttStrNextSegment(&topicCursor, &topicSegment)) {
            return false;
        }

        if (mqttStrEqCStr(patternSegment, "+")) {
            capture(captures, maxCaptures, &captured, topicSegment);
        } else if (!mqttStrEq(patternSegment, topicSegment)) {
            return false;
        }
    }

    /* Anchored at the far end too: a topic with segments the pattern never
       accounted for is not a match. */
    MqttString leftover;
    if (mqttStrNextSegment(&topicCursor, &leftover)) {
        return false;
    }

    if (captureCount != NULL) {
        *captureCount = captured;
    }

    return true;
}

bool mqttTopicMatchUnder(MqttString topic, const char* prefix, const char* pattern,
                         MqttString* captures, uint8_t maxCaptures, uint8_t* captureCount) {
    MqttString rest;
    if (!mqttStrStripTopicPrefixCStr(topic, prefix, &rest)) {
        if (captureCount != NULL) {
            *captureCount = 0;
        }

        return false;
    }

    return mqttTopicMatch(rest, pattern, captures, maxCaptures, captureCount);
}

bool mqttStrToU32(MqttString s, uint32_t* out) {
    if (mqttStrIsEmpty(s)) {
        return false;
    }

    uint32_t value = 0;
    for (uint16_t i = 0; i < s.length; i++) {
        char c = s.data[i];
        if (c < '0' || c > '9') {
            return false;
        }

        /* Rearranged so the check happens before the multiply rather than
           after it, where the overflow would already have wrapped. */
        uint32_t digit = (uint32_t)(c - '0');
        if (value > (UINT32_MAX - digit) / 10u) {
            return false;
        }

        value = value * 10u + digit;
    }

    *out = value;
    return true;
}
