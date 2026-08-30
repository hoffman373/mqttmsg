/**
 * @file mqtt.h
 * @brief MQTT 3.1 frame encoders and the wire-format parser.
 *
 * The transport-independent half of the library: everything here turns C
 * values into wire bytes or wire bytes into views, and none of it touches a
 * socket. It builds and is tested on the host.
 *
 * @par Ownership
 * The `build*()` functions allocate; the caller releases the result with
 * mqttPayloadFree(). A MqttPayload passed @e into a builder is copied, and the
 * caller still owns that too. parseMessage() allocates nothing at all.
 *
 * @par Checking the result
 * A builder that could not produce a frame returns a failure rather than
 * something that looks like a short frame, so every result is worth a
 * mqttPayloadIsOk() before it is used. A failure handed back into any of
 * these functions is refused and comes out as
 * ::PayloadFailBadArgs — it never becomes a frame on the wire. See
 * mqtt_payload.h.
 *
 * @code
 * MqttPayload body  = makeStringPayload("21.5");
 * MqttPayload frame = buildPublish("home/temp", 0, body, false);
 * mqttPayloadFree(&body);
 * if (mqttPayloadIsOk(frame)) {
 *   // ... send frame ...
 * }
 * mqttPayloadFree(&frame);
 * @endcode
 */

#ifndef MQTTMSG_MQTT_H
#define MQTTMSG_MQTT_H

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "mqtt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Writes a 16-bit value to @p dest in MQTT's big-endian byte order.
 * @param dest  Destination, which must have room for two bytes.
 * @param value Value to encode.
 */
void mqttShortToBytes(uint8_t* dest, uint16_t value);

/**
 * @brief Whether @p connFlags has anything set, and so needs a flags byte.
 * @param connFlags Flags to test.
 * @return true if the CONNECT flags byte must be emitted.
 */
bool includeConnFlags(ConnectFlags connFlags);

/**
 * @brief Encodes a C string as a length-prefixed MQTT string.
 *
 * Two big-endian length bytes followed by the characters, which is how
 * strings appear on the wire.
 *
 * @param strToConvert NUL-terminated source string.
 * @return Newly allocated MqttPayload of `strlen + 2` bytes; release it with
 *         mqttPayloadFree(). ::PayloadFailOom if the allocation failed.
 */
MqttPayload mqttMakeString(const char* strToConvert);

/**
 * @brief Copies a C string into a MqttPayload with no length prefix.
 *
 * The form a PUBLISH body takes: raw bytes, no framing.
 *
 * @param strToConvert NUL-terminated source string.
 * @return Newly allocated MqttPayload of `strlen` bytes; release it with
 *         mqttPayloadFree(). ::PayloadFailOom if the allocation failed. An
 *         empty string yields an Ok, empty MqttPayload, not a None one — a
 *         zero-length body is how a retained topic gets cleared, and it has
 *         to survive the round trip to the wire.
 */
MqttPayload makeStringPayload(const char* strToConvert);

/**
 * @brief Builds a CONNECT frame.
 *
 * Protocol level 3 ("MQIsdp"), clean session and will QoS 1 are fixed by
 * this builder and not parameterised.
 *
 * @param clientId         Client identifier; required.
 * @param username         Username, or NULL to omit it and its flag.
 * @param password         Password, or NULL to omit it and its flag.
 * @param willTopic        Last-will topic, or NULL for no will.
 * @param willMessage      Last-will payload; ignored when @p willTopic is
 *                         NULL.
 * @param willRetain       Whether the broker retains the will message.
 * @param keepAliveSeconds How long the broker should let this client go
 *                         quiet, or 0 to ask it never to time the client
 *                         out. Whatever is passed here is what the broker
 *                         holds the client to — it may hang up after one
 *                         and a half times this — so it has to agree with
 *                         how often the client actually sends something.
 * @return Newly allocated frame; release it with mqttPayloadFree(). A
 *         failure if it could not be built.
 */
MqttPayload buildConnect(const char* clientId, const char* username, const char* password,
                         const char* willTopic, const char* willMessage, bool willRetain,
                         uint16_t keepAliveSeconds);

/**
 * @brief Builds a PUBLISH frame.
 *
 * The QoS follows @p messageId, because MQTT couples them: a PUBLISH above
 * QoS 0 must carry a packet identifier, and only one at QoS 0 may omit it.
 *
 * @param topic     Topic to publish to.
 * @param messageId Packet identifier. Non-zero builds a QoS 1 PUBLISH
 *                  carrying it; 0 builds a QoS 0 PUBLISH with no identifier
 *                  field.
 * @param payload   Message body, Ok or None. Copied into the frame — the
 *                  caller still owns @p payload and must free it. A failure
 *                  is refused rather than encoded.
 * @param isRetain  Whether the broker retains the message.
 * @return Newly allocated frame; release it with mqttPayloadFree(). A
 *         failure if it could not be built.
 */
MqttPayload buildPublish(const char* topic, uint16_t messageId, MqttPayload payload, bool isRetain);

/**
 * @brief Builds a PUBACK frame, acknowledging a QoS 1 PUBLISH.
 * @param messageId Identifier of the message being acknowledged.
 * @return Newly allocated frame; release it with mqttPayloadFree(). A
 *         failure if it could not be built.
 */
MqttPayload buildPublishAck(uint16_t messageId);

/**
 * @brief Builds a PUBREC frame, the first QoS 2 acknowledgement.
 * @param messageId Identifier of the message being acknowledged.
 * @return Newly allocated frame; release it with mqttPayloadFree(). A
 *         failure if it could not be built.
 */
MqttPayload buildPubRec(uint16_t messageId);

/**
 * @brief Builds a PUBREL frame, the second step of the QoS 2 handshake.
 * @param messageId Identifier of the message being released.
 * @return Newly allocated frame; release it with mqttPayloadFree(). A
 *         failure if it could not be built.
 */
MqttPayload buildPubRel(uint16_t messageId);

/**
 * @brief Builds a PUBCOMP frame, completing the QoS 2 handshake.
 * @param messageId Identifier of the message being completed.
 * @return Newly allocated frame; release it with mqttPayloadFree(). A
 *         failure if it could not be built.
 */
MqttPayload buildPubComp(uint16_t messageId);

/**
 * @brief Builds a SUBSCRIBE frame carrying one or more topic filters.
 * @param messageId     Identifier the matching SUBACK will echo.
 * @param subscriptions Array of topic/QoS pairs.
 * @param subCount      Number of entries in @p subscriptions.
 * @return Newly allocated frame; release it with mqttPayloadFree(). A
 *         failure if it could not be built.
 */
MqttPayload buildSubscribe(uint16_t messageId, Subscription* subscriptions, int subCount);

/**
 * @brief Builds an UNSUBSCRIBE frame.
 * @param messageId     Identifier the matching UNSUBACK will echo.
 * @param subscriptions Array of topic filters. No QoS byte is sent.
 * @param subCount      Number of entries in @p subscriptions.
 * @return Newly allocated frame; release it with mqttPayloadFree(). A
 *         failure if it could not be built.
 */
MqttPayload buildUnsubscribe(uint16_t messageId, const char** subscriptions, int subCount);

/**
 * @brief Builds a PINGREQ frame.
 * @return Newly allocated frame; release it with mqttPayloadFree(). A
 *         failure if it could not be built.
 */
MqttPayload buildPingReq(void);

/**
 * @brief Builds a DISCONNECT frame.
 * @return Newly allocated frame; release it with mqttPayloadFree(). A
 *         failure if it could not be built.
 */
MqttPayload buildDisconnectMsg(void);

/**
 * @brief Parses a frame body in place.
 * @param fixedHeader Header already read by parseFixedHeader().
 * @param toParse     Bytes of the frame. A @p toParse that is not
 *                    mqttPayloadIsOk() yields ::MessageIncomplete for a
 *                    None and ::MessageMalformed for a failure.
 * @return The parsed message. Check Message::status before using any field.
 * @warning The result borrows @p toParse. Its strings and cursors point
 *          into `toParse.buffer` and are valid only while those bytes are.
 *          Nothing is allocated and there is no matching free function.
 * @see parseFixedHeader
 */
Message parseMessage(FixedHeader fixedHeader, MqttPayload toParse);

/**
 * @brief Reads the CONNECT strings out of a parsed message.
 * @param m   Parsed message.
 * @param out Receives the client id, will and credential strings.
 * @return false unless @p m parsed cleanly and really is a CONNECT.
 * @warning @p out borrows the same buffer @p m does.
 */
bool mqttParseConnectPayload(const Message* m, ConnectPayload* out);

/**
 * @brief Reads the subscription list out of a parsed SUBSCRIBE.
 * @param m   Parsed message.
 * @param out Receives a cursor over the topic/QoS pairs and their count.
 * @return false unless @p m parsed cleanly and really is a SUBSCRIBE.
 * @see mqttNextSubscription
 */
bool mqttParseSubscriptions(const Message* m, SubscriptionPayload* out);

/**
 * @brief Reads the topic list out of a parsed UNSUBSCRIBE.
 * @param m   Parsed message.
 * @param out Receives a cursor over the topics and their count.
 * @return false unless @p m parsed cleanly and really is an UNSUBSCRIBE.
 * @see mqttNextUnsubTopic
 */
bool mqttParseUnsubTopics(const Message* m, UnsubscribePayload* out);

/**
 * @brief Reads the granted QoS bytes out of a parsed SUBACK.
 * @param m   Parsed message.
 * @param out Receives the granted-QoS array and its length.
 * @return false unless @p m parsed cleanly and really is a SUBACK.
 * @see mqttGrantedQosAt
 */
bool mqttParseGrantedQos(const Message* m, SubscriptionResponsePayload* out);

/**
 * @brief Advances a SUBSCRIBE cursor to the next topic/QoS pair.
 * @param cursor Cursor to advance; updated in place.
 * @param out    Receives the next subscription.
 * @return false when the entries are exhausted or the frame is malformed.
 *         Message::status distinguishes the two at parse time.
 */
bool mqttNextSubscription(MqttSubscriptionCursor* cursor, MqttSubscription* out);

/**
 * @brief Advances an UNSUBSCRIBE cursor to the next topic.
 * @param cursor Cursor to advance; updated in place.
 * @param out    Receives the next topic.
 * @return false when the topics are exhausted or the frame is malformed.
 */
bool mqttNextUnsubTopic(MqttTopicCursor* cursor, MqttString* out);

/**
 * @brief Granted QoS for one subscription of a SUBACK.
 * @param granted Granted-QoS array from mqttParseGrantedQos().
 * @param index   Zero-based subscription index.
 * @return The granted QoS, or ::AtMostOnce if @p index is out of range —
 *         check `granted.grantedCount` to tell those apart. The wire value
 *         0x80, meaning the broker refused the subscription, is passed
 *         through unchanged.
 */
QoS mqttGrantedQosAt(SubscriptionResponsePayload granted, int index);

/**
 * @brief Parses the fixed header at the front of a frame.
 *
 * The first step in reading anything off the wire: it yields the message
 * type and the remaining length, which is how many more bytes belong to
 * this frame.
 *
 * @param toParse Bytes received so far. May hold less than a whole frame.
 * @return The header. Check FixedHeader::status — ::FixedHeaderIncomplete
 *         means wait for more bytes, ::FixedHeaderInvalid means the stream
 *         is corrupt. A @p toParse that is itself a failure is reported as
 *         ::FixedHeaderInvalid: no later byte makes it readable.
 */
FixedHeader parseFixedHeader(MqttPayload toParse);

#ifdef __cplusplus
}
#endif

#endif
