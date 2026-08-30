/**
 * @file mqtt_types.h
 * @brief The type vocabulary shared by the encoders, the parser and both
 *        clients.
 *
 * Header-only: no functions are declared here. The operations on these
 * types live in mqtt.h.
 */

#ifndef MQTTMSG_MQTT_TYPES_H
#define MQTTMSG_MQTT_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "mqtt_payload.h"
#include "mqtt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief MQTT control packet type, as encoded in the high nibble of byte 0. */
typedef enum {
    Connect = 1,      /**< Client request to connect to a broker. */
    ConnAck = 2,      /**< Broker's response to a CONNECT. */
    Publish = 3,      /**< Message delivery, in either direction. */
    PubAck = 4,       /**< QoS 1 acknowledgement. */
    PubRec = 5,       /**< QoS 2, step one: publish received. */
    PubRel = 6,       /**< QoS 2, step two: publish released. */
    PubComp = 7,      /**< QoS 2, step three: publish complete. */
    Subscribe = 8,    /**< Client request to subscribe to topic filters. */
    SubAck = 9,       /**< Broker's response to a SUBSCRIBE. */
    Unsubscribe = 10, /**< Client request to drop subscriptions. */
    UnSubAck = 11,    /**< Broker's response to an UNSUBSCRIBE. */
    PingReq = 12,     /**< Keep-alive request. */
    PingResp = 13,    /**< Keep-alive response. */
    Disconnect = 14   /**< Client is closing the session cleanly. */
} MessageType;

/** @brief Delivery guarantee for a published message. */
typedef enum {
    AtMostOnce = 0,  /**< QoS 0: fire and forget. */
    AtLeastOnce = 1, /**< QoS 1: acknowledged, may be duplicated. */
    ExactlyOnce = 2  /**< QoS 2: four-way handshake, never duplicated. */
} QoS;

/** @brief How far parseFixedHeader() got. */
typedef enum {
    FixedHeaderOk = 0,
    /**
     * Not enough bytes have arrived yet to finish the remaining-length
     * field. Wait for more data rather than treating this as an error.
     */
    FixedHeaderIncomplete = 1,
    /**
     * The remaining-length field exceeded the MQTT-mandated 4-byte encoding
     * without terminating. The stream is corrupt, not merely incomplete.
     */
    FixedHeaderInvalid = 2
} FixedHeaderStatus;

/** @brief The two-or-more byte header every MQTT frame starts with. */
typedef struct {
    MessageType type;         /**< Control packet type. */
    bool isDup;               /**< Redelivery flag; PUBLISH only. */
    QoS qos;                  /**< Delivery guarantee; PUBLISH only. */
    bool isRetain;            /**< Retain flag; PUBLISH only. */
    uint32_t length;          /**< Remaining length: bytes after this header. */
    uint8_t lenBytes;         /**< How many bytes the length field occupied, 1-4. */
    FixedHeaderStatus status; /**< Check this before trusting the rest. */
} FixedHeader;

/** @brief CONNACK return code. */
typedef enum {
    Accepted = 0,             /**< Connection accepted. */
    UnacceptableProtocol = 1, /**< Broker does not support this protocol level. */
    IdentifiedRejected = 2,   /**< Client identifier rejected. */
    ServerUnavailable = 3,    /**< Broker is up but unavailable. */
    BadUserNamePassword = 4,  /**< Credentials malformed or wrong. */
    NotAuthorized = 5,        /**< Client is not authorised to connect. */
    UNSET = 256               /**< No code present; the field is absent. */
} ConnectReturn;

/** @brief The CONNECT flags byte, unpacked. */
typedef struct {
    bool isPassword;     /**< A password field follows in the payload. */
    bool isUserName;     /**< A username field follows in the payload. */
    bool isRetain;       /**< The broker should retain the will message. */
    QoS willQoS;         /**< Delivery guarantee for the will message. */
    bool isWill;         /**< A will topic and message follow in the payload. */
    bool isCleanSession; /**< Discard any session state held for this client. */
} ConnectFlags;

/** @brief The variable header, whose fields differ by message type. */
typedef struct {
    bool isProtocolNameIncluded; /**< CONNECT: the "MQIsdp" string is present. */
    uint8_t version;             /**< CONNECT: protocol level. */
    ConnectFlags connFlags;      /**< CONNECT: the flags byte. */
    uint16_t keepAlive;          /**< CONNECT: keep-alive interval, seconds. */
    ConnectReturn connReturn;    /**< CONNACK: return code, or ::UNSET. */
    /**
     * PUBLISH only. Borrows the receive buffer; see the Message warning.
     */
    MqttString topicName;
    uint16_t messageId; /**< Identifier tying a request to its ack. */
} VariableHeader;

/**
 * @brief Build-side input to buildSubscribe().
 *
 * Deliberately `const char*` rather than MqttString: callers pass ordinary
 * C strings, usually literals. See MqttSubscription for the parse-side
 * counterpart.
 */
typedef struct {
    const char* topic; /**< Topic filter to subscribe to. */
    QoS qos;           /**< Requested delivery guarantee. */
} Subscription;

/** @brief Parse-side counterpart to Subscription, yielded by the cursor. */
typedef struct {
    MqttString topic; /**< Topic filter, borrowing the receive buffer. */
    QoS qos;          /**< Requested delivery guarantee. */
} MqttSubscription;

/**
 * @brief Cursor over the topic/QoS pairs in a parsed SUBSCRIBE.
 *
 * Walk it with mqttNextSubscription(); nothing is allocated.
 *
 * @code
 * MqttSubscriptionCursor cursor = subs.cursor;
 * MqttSubscription subscription;
 * while (mqttNextSubscription(&cursor, &subscription)) { ... }
 * @endcode
 *
 * @note Copy the cursor before walking if you need to iterate twice.
 */
typedef struct {
    const uint8_t* cur; /**< Next unread byte. */
    const uint8_t* end; /**< One past the last byte of the list. */
} MqttSubscriptionCursor;

/** @brief The payload of a parsed SUBSCRIBE. */
typedef struct {
    MqttSubscriptionCursor cursor; /**< Cursor over the entries. */
    int subscriptionCount;         /**< How many entries the frame holds. */
} SubscriptionPayload;

/**
 * @brief How far parseMessage() got.
 *
 * The distinction matters to a caller reading from a stream:
 * ::MessageIncomplete means the bytes are fine as far as they go and the
 * rest has not arrived, so wait and retry with more; ::MessageMalformed
 * means the frame contradicts itself and no amount of waiting will help,
 * so the connection should be dropped.
 */
typedef enum {
    MessageOk = 0,
    MessageIncomplete = 1, /**< Wait for more bytes and parse again. */
    MessageMalformed = 2   /**< Self-contradictory frame; drop the connection. */
} MessageStatus;

/**
 * @brief The payload of a parsed SUBACK.
 *
 * Granted QoS arrives as one byte per subscription, so the payload bytes
 * are the array. Read them with mqttGrantedQosAt().
 */
typedef struct {
    const uint8_t* grantedQos; /**< One byte per subscription. */
    int grantedCount;          /**< How many bytes @c grantedQos holds. */
} SubscriptionResponsePayload;

/**
 * @brief Cursor over the topic list in a parsed UNSUBSCRIBE.
 *
 * Walk it with mqttNextUnsubTopic().
 */
typedef struct {
    const uint8_t* cur; /**< Next unread byte. */
    const uint8_t* end; /**< One past the last byte of the list. */
} MqttTopicCursor;

/** @brief The payload of a parsed UNSUBSCRIBE. */
typedef struct {
    MqttTopicCursor cursor; /**< Cursor over the topics. */
    int topicCount;         /**< How many topics the frame holds. */
} UnsubscribePayload;

/** @brief The payload of a parsed CONNECT, as views over the receive buffer. */
typedef struct {
    MqttString clientId;    /**< Client identifier; always present. */
    MqttString username;    /**< Username, absent unless the flag was set. */
    MqttString willTopic;   /**< Will topic, absent unless the flag was set. */
    MqttString willMessage; /**< Will payload, absent unless the flag was set. */
    MqttString password;    /**< Password, absent unless the flag was set. */
} ConnectPayload;

/** @brief A received PUBLISH, as handed to an MqttPubCallback. */
typedef struct {
    FixedHeader fixedHeader;       /**< Type, QoS, retain and length. */
    VariableHeader variableHeader; /**< Topic name and message identifier. */
    MqttPayload payload;           /**< Message body. */
} MqttMessage;

/**
 * @brief A parsed MQTT message.
 *
 * Carries what every message type has. The parts specific to one type —
 * the CONNECT strings, the SUBSCRIBE list, the granted QoS bytes — are not
 * here; ask for them with the matching `mqttParse*()` accessor in mqtt.h,
 * which reads them out of #payload on demand and refuses if the message is
 * not that type. That keeps a Message small on the receive path, where it
 * is a stack temporary zeroed for every frame that arrives.
 *
 * @warning A Message owns nothing. Every string and payload in it borrows
 *          the buffer handed to parseMessage(), and stays valid only as
 *          long as that buffer holds the same bytes — for a received
 *          message, the duration of the callback, since the next read
 *          overwrites it. There is nothing to free, and freeing any of it
 *          is a bug. Copy anything you need to keep: mqttStrToCStr() for
 *          strings, a plain memcpy for payload bytes.
 * @warning Check #status before trusting any field.
 */
typedef struct {
    FixedHeader fixedHeader;       /**< Type, flags and remaining length. */
    VariableHeader variableHeader; /**< Type-dependent header fields. */
    MqttPayload payload;           /**< Remaining bytes, for the accessors. */
    MessageStatus status;          /**< Whether the parse succeeded. */
} Message;

#ifdef __cplusplus
}
#endif

#endif
