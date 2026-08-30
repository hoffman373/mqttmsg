/**
 * @file framing.c
 * @brief The receive-side framing loop: byte stream in, whole messages out.
 *
 * @see framing.h
 */

#include "framing.h"

#include <string.h>

#include <mqttmsg/mqtt.h>

FramingStatus framerConsume(MessageFramer* framer, int addedBytes) {
    framer->length += addedBytes;

    MqttPayload pay = mqttPayloadFromBytes(framer->buffer, (uint32_t)framer->length);
    FixedHeader fh = parseFixedHeader(pay);

    while (fh.status == FixedHeaderOk) {
        int msgLenTotal = (int)fh.length + 1 + fh.lenBytes;
        if (msgLenTotal > framer->length) {
            /* Header is complete, but the rest of the message hasn't arrived. */
            break;
        }

        /* The handler sees exactly one frame, not everything buffered behind
           it: a parse bounded by the read length rather than the frame length
           would run a short frame into the head of the next one. */
        pay = mqttPayloadFromBytes(framer->buffer, (uint32_t)msgLenTotal);
        framer->onMessage(framer->ctx, &fh, &pay);

        int newLength = framer->length - msgLenTotal;
        if (newLength <= 0) {
            framer->length = 0;
            break;
        }

        memmove(framer->buffer, framer->buffer + msgLenTotal, (size_t)newLength);
        framer->length = newLength;

        pay = mqttPayloadFromBytes(framer->buffer, (uint32_t)framer->length);
        fh = parseFixedHeader(pay);
    }

    if (fh.status == FixedHeaderInvalid) {
        framer->length = 0;
        return FramingMalformed;
    }

    return FramingOk;
}

FramingStatus framerFeed(MessageFramer* framer, const uint8_t* data, int length) {
    if (length > framerRemaining(framer)) {
        return FramingOverflow;
    }

    memcpy(framer->buffer + framer->length, data, (size_t)length);
    return framerConsume(framer, length);
}
