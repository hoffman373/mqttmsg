/**
 * @file mqtt.c
 * @brief MQTT 3.1 frame encoders and the wire-format parser.
 *
 * The transport-independent core. Encoding runs each payload writer twice —
 * once against a measuring PayloadWriter to size the allocation, once
 * against a real one to fill it — so the size and the bytes can never
 * disagree. Parsing allocates nothing and returns views into the caller's
 * buffer.
 *
 * @see mqtt.h
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <mqttmsg/mqtt.h>

#include "payload_writer.h"
#include <mqttmsg/logger.h>

void mqttShortToBytes(uint8_t* dest, uint16_t value) {
    dest[0] = (value >> 8) & 0xff;
    dest[1] = value & 0xff;
}

bool includeConnFlags(ConnectFlags connFlags) {
    return connFlags.isPassword || connFlags.isUserName || connFlags.isRetain ||
           connFlags.willQoS != AtMostOnce || connFlags.isWill || connFlags.isCleanSession;
}

/**
 * @brief Allocates a buffer for a payload of @p length bytes.
 *
 * One byte is always requested, even for an empty payload: malloc(0) may
 * hand back NULL, which would be indistinguishable from running out of
 * memory, and an empty body has to come back Ok rather than as a failure.
 *
 * @param length Bytes the payload needs to hold.
 * @return The buffer, or NULL if the allocation failed.
 */
static uint8_t* allocPayloadBuffer(size_t length) { return malloc(length > 0 ? length : 1); }

MqttPayload mqttMakeString(const char* strToConvert) {
    size_t len = strlen(strToConvert) + 2;
    uint8_t* buffer = allocPayloadBuffer(len);
    if (buffer == NULL) {
        return mqttPayloadFailure(PayloadFailOom);
    }

    PayloadWriter w = writerOver(buffer, len);
    putString(&w, strToConvert);
    return mqttPayloadFromBytes(buffer, (uint32_t)len);
}

MqttPayload makeStringPayload(const char* strToConvert) {
    size_t len = strlen(strToConvert);
    uint8_t* buffer = allocPayloadBuffer(len);
    if (buffer == NULL) {
        return mqttPayloadFailure(PayloadFailOom);
    }

    memcpy(buffer, strToConvert, len);
    return mqttPayloadFromBytes(buffer, (uint32_t)len);
}

/**
 * @brief The largest remaining length MQTT can express.
 *
 * Four bytes of seven bits each. A frame longer than this has no encoding,
 * so buildMessage() refuses it rather than emitting a field that cannot be
 * read back.
 */
#define MQTT_MAX_REMAINING_LENGTH 268435455u

/**
 * @brief How many bytes the remaining-length field needs for @p
 *        messageLength.
 * @param messageLength Length to encode, at most
 *        ::MQTT_MAX_REMAINING_LENGTH.
 * @return 1 to 4, the range MQTT allows.
 */
static int bytesToEncodeLength(unsigned int messageLength) {
    if (messageLength < 128) {
        return 1;
    } else if (messageLength < 16384) {
        return 2;
    } else if (messageLength < 2097152) {
        return 3;
    } else {
        return 4;
    }
}

/**
 * @brief Encodes a remaining-length field.
 *
 * Seven bits per byte, with the top bit set on every byte but the last.
 *
 * @param bytesToSend Length to encode. Must be at most
 *        ::MQTT_MAX_REMAINING_LENGTH, which is the point at which the
 *        encoding runs to a fifth byte and the four the buffer holds are
 *        no longer enough; buildMessage() is where that is enforced.
 * @return Newly allocated buffer of 1 to 4 bytes; release it with
 *         mqttPayloadFree(). ::PayloadFailOom if allocation failed.
 */
static MqttPayload buildMessageLength(unsigned int bytesToSend) {
    unsigned int len = bytesToEncodeLength(bytesToSend);
    /* The buffer is checked before it becomes a MqttPayload, so there is never a
       moment where one exists with a length it cannot back. */
    uint8_t* buffer = malloc(len);
    if (buffer == NULL) {
        return mqttPayloadFailure(PayloadFailOom);
    }

    int i = 0;
    do {
        int digit = bytesToSend % 128;
        bytesToSend = bytesToSend / 128;
        // if there are more digits to encode, set the top bit of this digit
        if (bytesToSend > 0) {
            digit = digit | 0x80;
        }

        buffer[i++] = digit;
    } while (bytesToSend > 0);

    return mqttPayloadFromBytes(buffer, len);
}

/**
 * @brief Writes the variable header, emitting only the fields that are set.
 *
 * Which fields a message type carries is expressed by which fields of
 * @p varHeader are non-zero, so one writer serves every type. Run it
 * against a measuring writer to size the frame.
 *
 * @param w         Writer to advance.
 * @param varHeader Header fields to emit.
 */
static void writeVarHeader(PayloadWriter* w, VariableHeader varHeader) {
    if (varHeader.isProtocolNameIncluded) {
        putString(w, "MQIsdp");
    }

    if (varHeader.version) {
        writeByte(w, varHeader.version);
    }

    if (includeConnFlags(varHeader.connFlags)) {
        uint8_t flags = varHeader.connFlags.isCleanSession ? 2 : 0;
        if (varHeader.connFlags.isWill) {
            flags |= 0b100;
            flags |= (varHeader.connFlags.willQoS << 3);
            flags |= (varHeader.connFlags.isRetain << 5);
        }

        flags |= (varHeader.connFlags.isPassword << 6);
        flags |= (varHeader.connFlags.isUserName << 7);
        writeByte(w, flags);
    }

    /* Keyed off the CONNECT marker rather than off the value: the keep-alive
       is two mandatory bytes of a CONNECT variable header, and zero is a
       meaningful value there — it asks the broker never to time the client
       out. Writing it only when non-zero would drop the field and shorten
       the header instead. */
    if (varHeader.isProtocolNameIncluded) {
        writeShort(w, varHeader.keepAlive);
    }

    if (varHeader.connReturn != UNSET) {
        writeByte(w, (uint8_t)varHeader.connReturn);
    }

    if (mqttStrIsPresent(varHeader.topicName)) {
        putView(w, varHeader.topicName);
    }

    if (varHeader.messageId) {
        writeShort(w, varHeader.messageId);
    }
}

/**
 * @brief Assembles a complete frame: fixed header, length, variable header,
 *        body.
 *
 * Every `build*()` function funnels through here.
 *
 * @param header    Type and flags for byte 0.
 * @param varHeader Variable header fields to emit.
 * @param payload   Message body, Ok or None. Copied; the caller still owns
 *                  it.
 * @return Newly allocated frame; release it with mqttPayloadFree(). A
 *         failure if @p payload was one, if allocation failed, or if the
 *         writer overflowed — the buffer is sized by writeVarHeader()
 *         itself, so an overflow means a bug, and a truncated frame would
 *         desync the broker's stream.
 */
static MqttPayload buildMessage(FixedHeader header, VariableHeader varHeader, MqttPayload payload) {
    /* A body, or the absence of one, is all that can be encoded. Every
       builder funnels through here, so this is where a payload that could not
       be built stops rather than becoming a frame. */
    if (!mqttPayloadIsOk(payload) && !mqttPayloadIsNone(payload)) {
        mqttmsgErrorPrint("Refusing to build a message from an unusable payload.\n");
        return mqttPayloadFailure(PayloadFailBadArgs);
    }

    PayloadWriter measure = writerMeasure();
    writeVarHeader(&measure, varHeader);
    uint64_t messageLength = (uint64_t)measure.writePos + mqttPayloadLength(payload);

    /* Above this the remaining-length field would need a fifth byte, which
       neither MQTT nor the encoder below has room for. Refusing here is what
       keeps buildMessageLength()'s four-byte buffer big enough. */
    if (messageLength > MQTT_MAX_REMAINING_LENGTH) {
        mqttmsgErrorPrint("Refusing to build a message longer than MQTT can express.\n");
        return mqttPayloadFailure(PayloadFailBadArgs);
    }

    MqttPayload encLength = buildMessageLength((unsigned int)messageLength);
    if (!mqttPayloadIsOk(encLength)) {
        /* Same reasoning: without the length field the frame would be missing
           the bytes that tell the broker how long it is. */
        mqttmsgErrorPrint("Out of memory encoding a remaining-length field.\n");
        return mqttPayloadFailure(PayloadFailOom);
    }

    int bufLen = (int)messageLength + (int)mqttPayloadLength(encLength) + 1;
    uint8_t* returnValue = malloc(bufLen);

    if (returnValue == NULL) {
        mqttPayloadFree(&encLength);
        return mqttPayloadFailure(PayloadFailOom);
    }

    PayloadWriter w = writerOver(returnValue, bufLen);

    uint8_t fixed = (header.type << 4);
    fixed |= (header.isDup << 3);
    fixed |= (header.qos << 1);
    fixed |= header.isRetain;
    writeByte(&w, fixed);

    writeBytes(&w, mqttPayloadBytes(encLength), mqttPayloadLength(encLength));
    writeVarHeader(&w, varHeader);
    writeBytes(&w, mqttPayloadBytes(payload), mqttPayloadLength(payload));

    mqttmsgTracePrint("pos %d payloadlength %d\n", (int)w.writePos,
                      (int)mqttPayloadLength(payload));

    mqttPayloadFree(&encLength);

    if (w.overflowed) {
        /* The buffer was sized by writeVarHeader itself, so reaching this
           means a bug rather than bad input. Drop the frame; a truncated one
           would desync the broker's stream. */
        mqttmsgErrorPrint("Frame buffer overflow while building a message.\n");
        free(returnValue);
        return mqttPayloadFailure(PayloadFailOverflow);
    }

    return mqttPayloadFromBytes(returnValue, (uint32_t)bufLen);
}

/**
 * @brief Writes the CONNECT payload strings.
 *
 * Field order is fixed by the spec: client id, will topic, will message,
 * username, password. A NULL argument omits that field.
 *
 * @param w           Writer to advance.
 * @param clientId    Client identifier; required.
 * @param username    Username, or NULL.
 * @param password    Password, or NULL.
 * @param willTopic   Will topic, or NULL.
 * @param willMessage Will payload, or NULL.
 */
static void writeConnectPayload(PayloadWriter* w, const char* clientId, const char* username,
                                const char* password, const char* willTopic,
                                const char* willMessage) {
    putString(w, clientId);

    if (willTopic) {
        putString(w, willTopic);
    }

    if (willMessage) {
        putString(w, willMessage);
    }

    if (username) {
        putString(w, username);
    }

    if (password) {
        putString(w, password);
    }
}

MqttPayload buildConnect(const char* clientId, const char* username, const char* password,
                         const char* willTopic, const char* willMessage, bool willRetain,
                         uint16_t keepAliveSeconds) {
    FixedHeader h;
    h.type = Connect;
    h.isDup = false;
    h.qos = AtMostOnce;
    h.isRetain = false;

    VariableHeader v;
    memset(&v, 0, sizeof(VariableHeader));
    v.isProtocolNameIncluded = true;
    v.version = 3;
    v.connFlags.isCleanSession = true;
    v.connFlags.isWill = willTopic;
    v.connFlags.willQoS = AtLeastOnce;
    v.connFlags.isRetain = willRetain;
    v.connFlags.isPassword = password != NULL;
    v.connFlags.isUserName = username != NULL;
    v.connReturn = UNSET;
    v.keepAlive = keepAliveSeconds;

    PayloadWriter measure = writerMeasure();
    writeConnectPayload(&measure, clientId, username, password, willTopic, willMessage);

    uint8_t* body = allocPayloadBuffer(measure.writePos);
    if (body == NULL) {
        return mqttPayloadFailure(PayloadFailOom);
    }

    MqttPayload payload = mqttPayloadFromBytes(body, (uint32_t)measure.writePos);
    PayloadWriter w = writerOver(body, measure.writePos);
    writeConnectPayload(&w, clientId, username, password, willTopic, willMessage);

    MqttPayload returnValue = buildMessage(h, v, payload);
    mqttPayloadFree(&payload);
    return returnValue;
}

MqttPayload buildPublish(const char* topic, uint16_t messageId, MqttPayload payload,
                         bool isRetain) {
    FixedHeader h;
    h.type = Publish;
    h.isDup = false;
    /* The packet identifier is what decides this, because the two travel
       together: a PUBLISH above QoS 0 must carry an identifier, and only a
       PUBLISH at QoS 0 may leave it out. writeVarHeader() emits the field
       only when it is non-zero, so deciding the QoS bits any other way
       produces a frame that contradicts itself. */
    h.qos = messageId ? AtLeastOnce : AtMostOnce;
    h.isRetain = isRetain;

    VariableHeader v;
    memset(&v, 0, sizeof(VariableHeader));
    v.connReturn = UNSET;
    v.topicName = mqttStrFromCStr(topic);
    v.messageId = messageId;

    /* The one builder taking a caller's payload, so the one that can be handed
       a failure. buildMessage() rejects it there rather than here, which keeps
       the check in the single place every builder funnels through. */
    return buildMessage(h, v, payload);
}

MqttPayload buildPublishAck(uint16_t messageId) {
    FixedHeader h;
    h.type = PubAck;
    h.isDup = false;
    /* The low nibble of byte 0 carries DUP/QoS/RETAIN only for PUBLISH. For
       every other type those bits are reserved, and only PUBREL, SUBSCRIBE
       and UNSUBSCRIBE set them (to 0b0010); PUBACK must leave them clear. */
    h.qos = AtMostOnce;
    h.isRetain = false;

    VariableHeader v;
    memset(&v, 0, sizeof(VariableHeader));
    v.connReturn = UNSET;
    v.messageId = messageId;

    MqttPayload payload = mqttPayloadNone();

    return buildMessage(h, v, payload);
}

MqttPayload buildPubRec(uint16_t messageId) {
    FixedHeader h;
    h.type = PubRec;
    h.isDup = false;
    h.qos = AtMostOnce;
    h.isRetain = false;

    VariableHeader v;
    memset(&v, 0, sizeof(VariableHeader));
    v.connReturn = UNSET;
    v.messageId = messageId;

    MqttPayload payload = mqttPayloadNone();

    return buildMessage(h, v, payload);
}

MqttPayload buildPubRel(uint16_t messageId) {
    FixedHeader h;
    h.type = PubRel;
    h.isDup = false;
    h.qos = AtLeastOnce;
    h.isRetain = false;

    VariableHeader v;
    memset(&v, 0, sizeof(VariableHeader));
    v.connReturn = UNSET;
    v.messageId = messageId;

    MqttPayload payload = mqttPayloadNone();

    return buildMessage(h, v, payload);
}

MqttPayload buildPubComp(uint16_t messageId) {
    FixedHeader h;
    h.type = PubComp;
    h.isDup = false;
    h.qos = AtMostOnce;
    h.isRetain = false;

    VariableHeader v;
    memset(&v, 0, sizeof(VariableHeader));
    v.connReturn = UNSET;
    v.messageId = messageId;

    MqttPayload payload = mqttPayloadNone();

    return buildMessage(h, v, payload);
}

/**
 * @brief Writes the SUBSCRIBE payload.
 *
 * Each entry is a length-prefixed topic followed by a QoS byte.
 *
 * @param w             Writer to advance.
 * @param subscriptions Array of topic/QoS pairs.
 * @param subCount      Number of entries.
 */
static void writeSubscribePayload(PayloadWriter* w, Subscription* subscriptions, int subCount) {
    for (int i = 0; i < subCount; i++) {
        putString(w, subscriptions[i].topic);
        writeByte(w, (uint8_t)subscriptions[i].qos);
    }
}

MqttPayload buildSubscribe(uint16_t messageId, Subscription* subscriptions, int subCount) {
    FixedHeader h;
    h.type = Subscribe;
    h.isDup = false;
    h.qos = AtLeastOnce;
    h.isRetain = false;

    VariableHeader v;
    memset(&v, 0, sizeof(VariableHeader));
    v.connReturn = UNSET;
    v.messageId = messageId;

    PayloadWriter measure = writerMeasure();
    writeSubscribePayload(&measure, subscriptions, subCount);

    uint8_t* body = allocPayloadBuffer(measure.writePos);
    if (body == NULL) {
        return mqttPayloadFailure(PayloadFailOom);
    }

    MqttPayload payload = mqttPayloadFromBytes(body, (uint32_t)measure.writePos);
    PayloadWriter w = writerOver(body, measure.writePos);
    writeSubscribePayload(&w, subscriptions, subCount);

    MqttPayload returnValue = buildMessage(h, v, payload);
    mqttPayloadFree(&payload);

    return returnValue;
}

/**
 * @brief Writes the UNSUBSCRIBE payload.
 *
 * Bare length-prefixed topics, with no QoS byte.
 *
 * @param w             Writer to advance.
 * @param subscriptions Array of topic filters.
 * @param subCount      Number of entries.
 */
static void writeUnsubscribePayload(PayloadWriter* w, const char** subscriptions, int subCount) {
    for (int i = 0; i < subCount; i++) {
        putString(w, subscriptions[i]);
    }
}

MqttPayload buildUnsubscribe(uint16_t messageId, const char** subscriptions, int subCount) {
    FixedHeader h;
    h.type = Unsubscribe;
    h.isDup = false;
    h.qos = AtLeastOnce;
    h.isRetain = false;

    VariableHeader v;
    memset(&v, 0, sizeof(VariableHeader));
    v.connReturn = UNSET;
    v.messageId = messageId;

    PayloadWriter measure = writerMeasure();
    writeUnsubscribePayload(&measure, subscriptions, subCount);

    uint8_t* body = allocPayloadBuffer(measure.writePos);
    if (body == NULL) {
        return mqttPayloadFailure(PayloadFailOom);
    }

    MqttPayload payload = mqttPayloadFromBytes(body, (uint32_t)measure.writePos);
    PayloadWriter w = writerOver(body, measure.writePos);
    writeUnsubscribePayload(&w, subscriptions, subCount);

    MqttPayload returnValue = buildMessage(h, v, payload);
    mqttPayloadFree(&payload);
    return returnValue;
}

MqttPayload buildPingReq(void) {
    FixedHeader h;
    h.type = PingReq;
    h.isDup = false;
    h.qos = AtMostOnce;
    h.isRetain = false;

    VariableHeader v;
    memset(&v, 0, sizeof(VariableHeader));
    v.connReturn = UNSET;

    MqttPayload payload = mqttPayloadNone();

    return buildMessage(h, v, payload);
}

MqttPayload buildDisconnectMsg(void) {
    FixedHeader h;
    h.type = Disconnect;
    h.isDup = false;
    h.qos = AtMostOnce;
    h.isRetain = false;

    VariableHeader v;
    memset(&v, 0, sizeof(VariableHeader));
    v.connReturn = UNSET;

    MqttPayload payload = mqttPayloadNone();

    return buildMessage(h, v, payload);
}

FixedHeader parseFixedHeader(MqttPayload toParse) {
    // Parse the fixed header
    FixedHeader h;
    h.length = 0;
    h.lenBytes = 0;

    if (!mqttPayloadIsOk(toParse) || toParse.length < 1) {
        h.type = 0;
        h.isDup = false;
        h.isRetain = false;
        h.qos = AtMostOnce;
        /* No bytes have arrived yet, or there were never going to be any: a
           failure is not a frame that waiting completes, so it is reported the
           way a desynced stream is rather than as "wait for more bytes". */
        h.status = mqttPayloadIsFailure(toParse) ? FixedHeaderInvalid : FixedHeaderIncomplete;
        return h;
    }

    h.type = toParse.buffer[0] >> 4;
    h.isDup = toParse.buffer[0] & 0b00001000;
    h.isRetain = toParse.buffer[0] & 1;
    h.qos = (toParse.buffer[0] >> 1) & 0b11;

    // Code remaining length. MQTT caps this encoding at 4 bytes, and it can
    // only be read as far as toParse actually reaches — the rest of the
    // stream may not have arrived yet.
    int multiplier = 1;
    uint8_t digit;
    do {
        if (h.lenBytes >= 4) {
            h.status = FixedHeaderInvalid;
            return h;
        }
        if ((uint32_t)(h.lenBytes + 1) >= toParse.length) {
            h.status = FixedHeaderIncomplete;
            return h;
        }
        h.lenBytes++;
        digit = toParse.buffer[h.lenBytes];
        h.length += (digit & 127) * multiplier;
        multiplier *= 128;
    } while ((digit & 128) != 0);

    h.status = FixedHeaderOk;
    return h;
}

/**
 * @brief Reads the length-prefixed string at @p at as a view.
 *
 * No copy, no allocation. Bounds-checked against @p end, so a frame that
 * claims a longer string than arrived — a truncated read, or a corrupt
 * length — is refused rather than read past the end of the buffer.
 *
 * @param at   First byte of the length prefix.
 * @param end  One past the last byte available.
 * @param out  Receives the string, or an empty view on failure.
 * @param next Receives the byte just past the string on success. On failure
 *             it receives where the read wanted to reach, which is what
 *             tells a caller whether the frame is merely unfinished or
 *             self-contradictory. May be NULL.
 * @return false if the string does not fit within @p end.
 */
static bool getViewAt(const uint8_t* at, const uint8_t* end, MqttString* out,
                      const uint8_t** next) {
    *out = mqttStrEmpty();

    if (at == NULL || end == NULL) {
        if (next != NULL) {
            *next = end;
        }
        return false;
    }

    /* On failure *next is where the read wanted to reach, which is what tells
       a caller whether the frame is merely unfinished or self-contradictory. */
    if (at + 2 > end) {
        if (next != NULL) {
            *next = at + 2;
        }
        return false;
    }

    uint16_t length = (uint16_t)at[0] << 8;
    length |= at[1];

    if (next != NULL) {
        *next = at + 2 + length;
    }

    if (at + 2 + length > end) {
        return false;
    }

    *out = mqttStrFromBytes((const char*)at + 2, length);
    return true;
}

/*
 * The three type-specific field walkers below are each used twice: by
 * parseMessage() to confirm a frame is complete before anything sees it,
 * and by the matching mqttParse*() accessor to hand the fields out. One
 * description of each layout, the same argument as the writer side.
 */

/**
 * @brief Walks the CONNECT payload strings.
 * @param at       First byte of the payload.
 * @param end      One past the last byte available.
 * @param flags    CONNECT flags, which say which fields are present.
 * @param out      Receives the strings, zeroed first.
 * @param failedAt On failure, receives where the read that did not fit
 *                 wanted to end.
 * @return false if the payload runs out before the flagged fields do.
 */
static bool walkConnectFields(const uint8_t* at, const uint8_t* end, ConnectFlags flags,
                              ConnectPayload* out, const uint8_t** failedAt) {
    memset(out, 0, sizeof(ConnectPayload));

    const uint8_t* cur = at;
    if (!getViewAt(cur, end, &out->clientId, &cur)) {
        *failedAt = cur;
        return false;
    }

    if (flags.isWill) {
        if (!getViewAt(cur, end, &out->willTopic, &cur) ||
            !getViewAt(cur, end, &out->willMessage, &cur)) {
            *failedAt = cur;
            return false;
        }
    }

    if (flags.isUserName && !getViewAt(cur, end, &out->username, &cur)) {
        *failedAt = cur;
        return false;
    }

    if (flags.isPassword && !getViewAt(cur, end, &out->password, &cur)) {
        *failedAt = cur;
        return false;
    }

    return true;
}

/**
 * @brief Walks the SUBSCRIBE topic/QoS pairs, counting them.
 * @param at       First byte of the payload.
 * @param end      One past the last byte available.
 * @param out      Receives a cursor over the entries and their count.
 * @param failedAt On failure, receives where the read that did not fit
 *                 wanted to end.
 * @return false if an entry is truncated. A topic with no QoS byte after it
 *         is a malformed entry, not the last one.
 */
static bool walkSubscriptions(const uint8_t* at, const uint8_t* end, SubscriptionPayload* out,
                              const uint8_t** failedAt) {
    memset(out, 0, sizeof(SubscriptionPayload));
    out->cursor.cur = at;
    out->cursor.end = end;

    const uint8_t* cur = at;
    while (cur != NULL && cur < end) {
        MqttString topic;
        const uint8_t* next = NULL;
        if (!getViewAt(cur, end, &topic, &next)) {
            *failedAt = next;
            return false;
        }

        /* Each entry is a topic plus one QoS byte; a topic with no QoS after
           it is a malformed entry, not the last one. */
        if (next >= end) {
            *failedAt = next + 1;
            return false;
        }

        cur = next + 1;
        out->subscriptionCount++;
    }

    return true;
}

/**
 * @brief Walks the UNSUBSCRIBE topic list, counting the entries.
 * @param at       First byte of the payload.
 * @param end      One past the last byte available.
 * @param out      Receives a cursor over the topics and their count.
 * @param failedAt On failure, receives where the read that did not fit
 *                 wanted to end.
 * @return false if a topic is truncated.
 */
static bool walkUnsubTopics(const uint8_t* at, const uint8_t* end, UnsubscribePayload* out,
                            const uint8_t** failedAt) {
    memset(out, 0, sizeof(UnsubscribePayload));
    out->cursor.cur = at;
    out->cursor.end = end;

    const uint8_t* cur = at;
    while (cur != NULL && cur < end) {
        MqttString topic;
        const uint8_t* next = NULL;
        if (!getViewAt(cur, end, &topic, &next)) {
            *failedAt = next;
            return false;
        }

        cur = next;
        out->topicCount++;
    }

    return true;
}

/* The type-specific accessors. Each re-reads its fields out of payload,
   which costs a few length-prefix reads over bytes already in the buffer,
   and refuses unless the message really is that type — so asking a PUBLISH
   for its CONNECT strings returns false rather than zeroed fields. */

bool mqttParseConnectPayload(const Message* m, ConnectPayload* out) {
    if (m == NULL || out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(ConnectPayload));
    if (m->status != MessageOk || m->fixedHeader.type != Connect || !mqttPayloadIsOk(m->payload)) {
        return false;
    }

    const uint8_t* failedAt = NULL;
    const uint8_t* at = mqttPayloadBytes(m->payload);
    return walkConnectFields(at, at + mqttPayloadLength(m->payload), m->variableHeader.connFlags,
                             out, &failedAt);
}

bool mqttParseSubscriptions(const Message* m, SubscriptionPayload* out) {
    if (m == NULL || out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(SubscriptionPayload));
    if (m->status != MessageOk || m->fixedHeader.type != Subscribe ||
        !mqttPayloadIsOk(m->payload)) {
        return false;
    }

    const uint8_t* failedAt = NULL;
    const uint8_t* at = mqttPayloadBytes(m->payload);
    return walkSubscriptions(at, at + mqttPayloadLength(m->payload), out, &failedAt);
}

bool mqttParseUnsubTopics(const Message* m, UnsubscribePayload* out) {
    if (m == NULL || out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(UnsubscribePayload));
    if (m->status != MessageOk || m->fixedHeader.type != Unsubscribe ||
        !mqttPayloadIsOk(m->payload)) {
        return false;
    }

    const uint8_t* failedAt = NULL;
    const uint8_t* at = mqttPayloadBytes(m->payload);
    return walkUnsubTopics(at, at + mqttPayloadLength(m->payload), out, &failedAt);
}

bool mqttParseGrantedQos(const Message* m, SubscriptionResponsePayload* out) {
    if (m == NULL || out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(SubscriptionResponsePayload));
    if (m->status != MessageOk || m->fixedHeader.type != SubAck) {
        return false;
    }

    /* One granted QoS byte per subscription, so the payload bytes already
       are the array. */
    out->grantedQos = mqttPayloadBytes(m->payload);
    out->grantedCount = (int)mqttPayloadLength(m->payload);
    return true;
}

bool mqttNextSubscription(MqttSubscriptionCursor* cursor, MqttSubscription* out) {
    if (cursor == NULL || out == NULL || cursor->cur == NULL || cursor->cur >= cursor->end) {
        return false;
    }

    const uint8_t* next = NULL;
    MqttString topic;
    if (!getViewAt(cursor->cur, cursor->end, &topic, &next)) {
        cursor->cur = cursor->end;
        return false;
    }

    /* Each entry is topic + one QoS byte; a topic with no QoS after it is a
       malformed frame, not a final entry. */
    if (next >= cursor->end) {
        cursor->cur = cursor->end;
        return false;
    }

    out->topic = topic;
    out->qos = (QoS)*next;
    cursor->cur = next + 1;
    return true;
}

bool mqttNextUnsubTopic(MqttTopicCursor* cursor, MqttString* out) {
    if (cursor == NULL || out == NULL || cursor->cur == NULL || cursor->cur >= cursor->end) {
        return false;
    }

    const uint8_t* next = NULL;
    if (!getViewAt(cursor->cur, cursor->end, out, &next)) {
        cursor->cur = cursor->end;
        return false;
    }

    cursor->cur = next;
    return true;
}

QoS mqttGrantedQosAt(SubscriptionResponsePayload granted, int index) {
    /* Out of range yields AtMostOnce rather than reading past the buffer;
       check grantedCount first. Note 0x80 is a legal wire value here meaning
       the broker refused the subscription, and is passed through as-is. */
    if (granted.grantedQos == NULL || index < 0 || index >= granted.grantedCount) {
        return AtMostOnce;
    }

    return (QoS)granted.grantedQos[index];
}

/**
 * @brief Classifies a read that ran out of room.
 *
 * Past what the frame itself says it contains means the frame contradicts
 * itself; past only what has arrived means the rest is still in flight.
 *
 * @param wanted   Where the failed read wanted to reach.
 * @param frameEnd End the remaining-length field claims, or NULL.
 * @param recvEnd  End of what actually arrived. Unused, and kept for
 *                 symmetry with the two bounds parseMessage() tracks.
 * @return ::MessageMalformed if @p wanted overran @p frameEnd, otherwise
 *         ::MessageIncomplete.
 */
static MessageStatus shortReadStatus(const uint8_t* wanted, const uint8_t* frameEnd,
                                     const uint8_t* recvEnd) {
    if (frameEnd != NULL && wanted > frameEnd) {
        return MessageMalformed;
    }

    (void)recvEnd;
    return MessageIncomplete;
}

/**
 * @brief Reads the two-byte message identifier at @p pos, bounds-checked.
 *
 * The message id is read at a fixed offset rather than through getViewAt(),
 * so it needs the same guard the string reads get: a frame whose
 * remaining-length field stops short of these two bytes must be refused, not
 * read past. Advances @p pos by two on success.
 *
 * @param toParse  The frame being parsed.
 * @param pos      Offset of the first id byte; advanced by two on success.
 * @param end      The nearer of the frame end and what arrived.
 * @param frameEnd End the remaining-length field claims.
 * @param recvEnd  End of what actually arrived.
 * @param out      Receives the identifier on success.
 * @param status   Set to the short-read classification on failure.
 * @return false if the two bytes do not both lie within @p end.
 */
static bool readMessageId(MqttPayload toParse, int* pos, const uint8_t* end,
                          const uint8_t* frameEnd, const uint8_t* recvEnd, uint16_t* out,
                          MessageStatus* status) {
    const uint8_t* wanted = toParse.buffer + *pos + 2;
    if (wanted > end) {
        *status = shortReadStatus(wanted, frameEnd, recvEnd);
        return false;
    }

    *out = (uint16_t)toParse.buffer[*pos] << 8;
    *out |= toParse.buffer[*pos + 1];
    *pos += 2;
    return true;
}

Message parseMessage(FixedHeader fixedHeader, MqttPayload toParse) {
    Message returnValue;
    memset(&returnValue, 0, sizeof(Message));
    returnValue.variableHeader.connReturn = UNSET;
    returnValue.fixedHeader = fixedHeader;
    returnValue.status = MessageOk;
    returnValue.payload = mqttPayloadNone();
    int pos = 1 + fixedHeader.lenBytes;

    if (!mqttPayloadIsOk(toParse)) {
        /* No bytes to read, so every branch below would be reading from NULL.
           A failure is reported as malformed rather than incomplete: it is not
           a frame that more data completes. */
        returnValue.status = mqttPayloadIsFailure(toParse) ? MessageMalformed : MessageIncomplete;
        return returnValue;
    }

    /* Two ends, and they can differ. recvEnd is what actually arrived;
       frameEnd is what the remaining-length field claims the frame contains.
       Reads are bounded by whichever is nearer, and which one stopped a read
       is what separates an incomplete frame from a malformed one. */
    const uint8_t* recvEnd = toParse.buffer + mqttPayloadLength(toParse);
    const uint8_t* frameEnd = toParse.buffer + 1 + fixedHeader.lenBytes + fixedHeader.length;
    const uint8_t* end = frameEnd < recvEnd ? frameEnd : recvEnd;

    switch (fixedHeader.type) {
        case Connect: {
            /* Protocol name, length-prefixed at the start of the variable header.
               Only "MQIsdp" is recognised; anything else parses fine but leaves
               isProtocolNameIncluded false. */
            MqttString protocolName;
            const uint8_t* afterName = NULL;
            if (!getViewAt(toParse.buffer + pos, end, &protocolName, &afterName)) {
                returnValue.status = shortReadStatus(afterName, frameEnd, recvEnd);
                break;
            }

            returnValue.variableHeader.isProtocolNameIncluded =
                mqttStrEqCStr(protocolName, "MQIsdp");

            /* Advance by the name actually present rather than a fixed width, so a
               3.1.1 CONNECT carrying the shorter "MQTT" does not desync the rest. */
            pos = (int)(afterName - toParse.buffer);

            /* version, connect flags, and two keep-alive bytes are read at fixed
               offsets below, so check they arrived before touching them. */
            if (afterName + 4 > end) {
                returnValue.status = shortReadStatus(afterName + 4, frameEnd, recvEnd);
                break;
            }

            // Extract version
            returnValue.variableHeader.version = toParse.buffer[pos++];

            // Extract conn flags.
            returnValue.variableHeader.connFlags.isUserName = toParse.buffer[pos] & 0b10000000;
            returnValue.variableHeader.connFlags.isPassword = toParse.buffer[pos] & 0b01000000;
            returnValue.variableHeader.connFlags.isRetain = toParse.buffer[pos] & 0b00100000;
            returnValue.variableHeader.connFlags.willQoS = (toParse.buffer[pos] & 0b00011000) >> 3;
            returnValue.variableHeader.connFlags.isWill = toParse.buffer[pos] & 0b00000100;
            returnValue.variableHeader.connFlags.isCleanSession = toParse.buffer[pos] & 0b00000010;

            // Keep alive.
            returnValue.variableHeader.keepAlive = toParse.buffer[++pos];
            returnValue.variableHeader.keepAlive = returnValue.variableHeader.keepAlive << 8;
            returnValue.variableHeader.keepAlive += toParse.buffer[++pos];

            pos++;

            returnValue.payload = mqttPayloadFromBytes(toParse.buffer + pos, toParse.length - pos);

            /* The strings stay in payload for mqttParseConnectPayload(); walking
               them here only confirms the frame is complete, so a truncated
               CONNECT never reaches a listener. */
            ConnectPayload fields;
            const uint8_t* failedAt = NULL;
            const uint8_t* body = mqttPayloadBytes(returnValue.payload);
            if (!walkConnectFields(body, body + mqttPayloadLength(returnValue.payload),
                                   returnValue.variableHeader.connFlags, &fields, &failedAt)) {
                returnValue.status = shortReadStatus(failedAt, frameEnd, recvEnd);
            }

            break;
        }

        case ConnAck: {
            /* Two bytes of variable header: acknowledge flags, then the return
               code read below. A frame whose remaining length stops short of
               them is refused rather than read past the end. */
            if (toParse.buffer + pos + 2 > end) {
                returnValue.status = shortReadStatus(toParse.buffer + pos + 2, frameEnd, recvEnd);
                break;
            }

            returnValue.payload = mqttPayloadNone();
            returnValue.variableHeader.connReturn = toParse.buffer[pos + 1];
            break;
        }

        case Publish: {
            const uint8_t* afterTopic = NULL;
            if (!getViewAt(toParse.buffer + pos, end, &returnValue.variableHeader.topicName,
                           &afterTopic)) {
                returnValue.status = shortReadStatus(afterTopic, frameEnd, recvEnd);
                break;
            }

            pos = (int)(afterTopic - toParse.buffer);

            if (fixedHeader.qos != AtMostOnce &&
                !readMessageId(toParse, &pos, end, frameEnd, recvEnd,
                               &returnValue.variableHeader.messageId, &returnValue.status)) {
                break;
            }

            returnValue.payload = mqttPayloadFromBytes(toParse.buffer + pos, toParse.length - pos);
            break;
        }

        case PubRec:
        case PubAck:
        case PubRel:
        case PubComp: {
            if (!readMessageId(toParse, &pos, end, frameEnd, recvEnd,
                               &returnValue.variableHeader.messageId, &returnValue.status)) {
                break;
            }

            returnValue.payload = mqttPayloadNone();
            break;
        }

        case Subscribe: {
            if (!readMessageId(toParse, &pos, end, frameEnd, recvEnd,
                               &returnValue.variableHeader.messageId, &returnValue.status)) {
                break;
            }

            returnValue.payload = mqttPayloadFromBytes(toParse.buffer + pos, toParse.length - pos);

            /* Entries stay in payload for mqttParseSubscriptions(); walking them
               here only rejects a malformed SUBSCRIBE before dispatch. */
            SubscriptionPayload entries;
            const uint8_t* failedAt = NULL;
            const uint8_t* body = mqttPayloadBytes(returnValue.payload);
            if (!walkSubscriptions(body, body + mqttPayloadLength(returnValue.payload), &entries,
                                   &failedAt)) {
                returnValue.status = shortReadStatus(failedAt, frameEnd, recvEnd);
            }

            break;
        }

        case SubAck: {
            if (!readMessageId(toParse, &pos, end, frameEnd, recvEnd,
                               &returnValue.variableHeader.messageId, &returnValue.status)) {
                break;
            }

            returnValue.payload = mqttPayloadFromBytes(toParse.buffer + pos, toParse.length - pos);

            break;
        }

        case Unsubscribe: {
            if (!readMessageId(toParse, &pos, end, frameEnd, recvEnd,
                               &returnValue.variableHeader.messageId, &returnValue.status)) {
                break;
            }

            returnValue.payload = mqttPayloadFromBytes(toParse.buffer + pos, toParse.length - pos);

            UnsubscribePayload topics;
            const uint8_t* failedAt = NULL;
            const uint8_t* body = mqttPayloadBytes(returnValue.payload);
            if (!walkUnsubTopics(body, body + mqttPayloadLength(returnValue.payload), &topics,
                                 &failedAt)) {
                returnValue.status = shortReadStatus(failedAt, frameEnd, recvEnd);
            }

            break;
        }

        case UnSubAck: {
            if (!readMessageId(toParse, &pos, end, frameEnd, recvEnd,
                               &returnValue.variableHeader.messageId, &returnValue.status)) {
                break;
            }

            returnValue.payload = mqttPayloadNone();
            break;
        }

        case Disconnect:
        case PingReq:
        case PingResp: {
            returnValue.payload = mqttPayloadNone();

            break;
        }
    }

    return returnValue;
}
