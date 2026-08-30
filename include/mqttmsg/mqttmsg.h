/**
 * @file mqttmsg.h
 * @brief The MQTT client: subscriptions, callbacks and publishing.
 *
 * One interface for both platforms. What differs is the transport
 * underneath it, which is chosen by including one more header and writing
 * three lines; everything after that is the same source on a Pico and on
 * Linux.
 *
 * @code
 * #include <mqttmsg/mqttmsg.h>
 * #include <mqttmsg/transport_pico.h>     // or transport_socket.h
 *
 * MqttPicoTransport* wire = mqttPicoTransportNew();
 * mqttPicoSetWifi(wire, WIFI_SSID, WIFI_PASSWORD);   // device only
 *
 * MqttClient* mqtt = mqttNew();
 * mqttPicoTransportAttach(wire, mqtt);
 *
 * mqttSetBroker(mqtt, MQTT_HOST, MQTT_PORT);
 * mqttSetCredentials(mqtt, MQTT_USERNAME, MQTT_PASSWORD);
 * mqttSetKeepAlive(mqtt, 90);
 * mqttSetWill(mqtt, "device/status", "offline", true);
 * mqttOnEvent(mqtt, ConnAck, onConnect);
 * mqttSubscribe(mqtt, "device/+/command", onCommand, NULL);
 *
 * while (true) { mqttPoll(mqtt); }
 * @endcode
 */

#ifndef MQTTMSG_MQTTMSG_H
#define MQTTMSG_MQTTMSG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <mqttmsg/mqtt_payload.h>
#include <mqttmsg/mqtt_string.h>
#include <mqttmsg/mqtt_types.h>

/** @brief An MQTT client. Created by mqttNew(). */
typedef struct MqttClient MqttClient;

/**
 * @brief Callback for a received frame.
 * @param client The client it arrived on.
 * @param frame  The whole frame, header included.
 * @warning @p frame points into the receive buffer, which the next read
 *          reuses. Copy anything you need to keep.
 */
typedef void (*MqttEventFn)(MqttClient* client, MqttPayload frame);

/**
 * @brief Callback for a PUBLISH matching a subscription.
 * @param client   The client it arrived on.
 * @param topic    The topic it was published to, which for a `+` or `#`
 *                 filter is what actually turned up rather than the filter.
 * @param body     The message body.
 * @param userData Whatever was passed to mqttSubscribe().
 * @warning @p topic and @p body are valid only for the duration of the
 *          call. Copy anything you need to keep.
 */
typedef void (*MqttMessageFn)(MqttClient* client, MqttString topic, MqttPayload body,
                              void* userData);

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/**
 * @brief Creates a client with no transport and nothing configured.
 * @return The new client, or NULL if allocation failed. Attach a transport
 *         before polling it.
 */
MqttClient* mqttNew(void);

/**
 * @brief Releases a client and its subscription table.
 * @param client Client to release, or NULL. Unusable afterwards. The
 *               transport is not freed; it was not this client's to make.
 */
void mqttFree(MqttClient* client);

/* ── Configuration ─────────────────────────────────────────────────── */

/**
 * @brief Sets the broker to connect to.
 * @param client Client to modify.
 * @param host   Hostname or dotted-quad. Stored by pointer, so it must
 *               outlive the client.
 * @param port   TCP port, conventionally 1883.
 */
void mqttSetBroker(MqttClient* client, const char* host, uint16_t port);

/**
 * @brief Sets the CONNECT credentials.
 * @param client   Client to modify.
 * @param username Username, or NULL for none.
 * @param password Password, or NULL for none.
 */
void mqttSetCredentials(MqttClient* client, const char* username, const char* password);

/**
 * @brief Sets the client identifier sent in CONNECT.
 *
 * Without this a random one is generated when the client is created and
 * reused across reconnects. That is unique, but a broker keying anything
 * off the identifier — a persistent session, an ACL — will not recognise
 * the same device twice across restarts.
 *
 * @param client   Client to modify.
 * @param clientId Identifier. Stored by pointer.
 */
void mqttSetClientId(MqttClient* client, const char* clientId);

/**
 * @brief Sets the last will, published if this client drops without saying
 *        goodbye.
 * @param client  Client to modify.
 * @param topic   Will topic, or NULL for no will. Stored by pointer.
 * @param message Will payload. Stored by pointer.
 * @param retain  Whether the broker retains it.
 */
void mqttSetWill(MqttClient* client, const char* topic, const char* message, bool retain);

/**
 * @brief Sets the keep-alive period, in seconds.
 *
 * One number does three jobs. It goes out in the CONNECT, which is what the
 * broker holds this client to — it may hang up after one and a half times
 * this with nothing heard. PINGREQs go out at half of it. And a session
 * that hears nothing for the broker's own one-and-a-half is torn down and
 * reconnected, which is how a connection that died without saying so gets
 * noticed rather than waited on forever.
 *
 * @param client  Client to modify.
 * @param seconds Keep-alive period. 90 is a reasonable default. Zero sends
 *                no pings and asks the broker never to time this client
 *                out.
 */
void mqttSetKeepAlive(MqttClient* client, uint16_t seconds);

/**
 * @brief Bounds how long mqttPoll() may wait for bytes.
 * @param client       Client to modify.
 * @param milliseconds How long to wait. Negative is treated as zero. What
 *                     it means is the transport's business: a socket waits
 *                     in the kernel, and a device whose stack runs in the
 *                     background ignores it.
 */
void mqttSetPollBudget(MqttClient* client, int milliseconds);

/**
 * @brief Attaches a caller context pointer.
 * @param client Client to modify.
 * @param data   Pointer to store. Not owned; not freed.
 */
void mqttSetData(MqttClient* client, void* data);

/**
 * @brief Retrieves the context pointer set by mqttSetData().
 * @param client Client to query.
 * @return The stored pointer, or NULL.
 */
void* mqttGetData(MqttClient* client);

/* ── Subscriptions and publishing ──────────────────────────────────── */

/**
 * @brief Registers a topic filter and the callback for it.
 *
 * Recorded up front and sent to the broker on every connect, so calling it
 * before the client is up is the normal case. The filter speaks the whole
 * MQTT vocabulary: `+` matches one level and `#` the rest.
 *
 * Registering `"atx/0/command"` through `"atx/3/command"` with a different
 * @p userData each is what saves the callback parsing the index back out of
 * the topic; a `+` filter, which cannot know what it will be sent, gets the
 * real topic instead.
 *
 * @param client   Client to modify.
 * @param filter   Topic filter. Stored by pointer, so it must outlive the
 *                 client.
 * @param onMessage Called for each matching PUBLISH.
 * @param userData Handed back to @p onMessage. Not owned; not freed.
 * @return true if it was recorded. false, having logged, if the table could
 *         not grow; what was already registered is left intact.
 */
bool mqttSubscribe(MqttClient* client, const char* filter, MqttMessageFn onMessage, void* userData);

/**
 * @brief Publishes a NUL-terminated string at QoS 1.
 *
 * Builds the frame, sends it and frees it; nothing is left to release.
 *
 * @param client  Client to publish on.
 * @param topic   Topic to publish to.
 * @param message Message body.
 * @param retain  Whether the broker retains the message.
 */
void mqttPublish(MqttClient* client, const char* topic, const char* message, bool retain);

/**
 * @brief Registers the callback for one message type.
 *
 * One function rather than a dozen setters, because most of the types have
 * nothing anybody wants to do with them. ::ConnAck and ::Disconnect are the
 * two worth registering for; PUBLISHes go to mqttSubscribe() instead.
 *
 * @param client Client to modify.
 * @param type   Which message type.
 * @param onEvent Called for each frame of that type, or NULL to stop.
 */
void mqttOnEvent(MqttClient* client, MessageType type, MqttEventFn onEvent);

/**
 * @brief Registers a callback for when nothing is happening.
 * @param client Client to modify.
 * @param onIdle Called from mqttPoll() when no frame is waiting, which is
 *               where a client does its own periodic work.
 */
void mqttOnIdle(MqttClient* client, MqttEventFn onIdle);

/* ── Running ───────────────────────────────────────────────────────── */

/**
 * @brief Advances the client by one step.
 *
 * Connects if nothing is up and the reconnect wait has passed, sends the
 * CONNECT, services the transport, dispatches whatever completes, and paces
 * keep-alive pings. Call it repeatedly; it returns.
 *
 * @param client Client to advance.
 */
void mqttPoll(MqttClient* client);

/**
 * @brief Whether the transport currently has a connection.
 *
 * Says nothing about whether the broker has accepted a CONNECT; that is
 * mqttSessionIsEstablished(). This is the cheaper question — can bytes
 * leave at all — which is what a caller watching for a dropped link wants.
 *
 * @param client Client to query.
 * @return true while the transport reports a connection.
 */
bool mqttIsConnected(MqttClient* client);

/**
 * @brief mqttPoll() in a loop, for a caller with nothing else to do.
 *
 * A broker that is down is not something it cannot retry: the transport
 * backs off and keeps trying, and this keeps polling it. What ends the
 * loop is a transport reporting ::MqttTransport::isStopped, or being
 * detached.
 *
 * @param client Client to run.
 * @warning Blocks. Returns only if the transport hits something it cannot
 *          retry.
 */
void mqttRun(MqttClient* client);

/**
 * @brief mqttPoll() with the signature RunLoopManager expects.
 * @param client The client, as a `void*`.
 */
void mqttPollRLM(void* client);

/* ── Writing a transport ───────────────────────────────────────────── */

/** @brief What became of one write. */
typedef enum {
    MqttWriteOk = 0,     /**< Every byte was handed over. */
    MqttWriteBusy = 1,   /**< Back-pressure. The message is lost, the connection is not. */
    MqttWriteFailed = 2, /**< The connection is broken and must be rebuilt. */
    MqttWriteNoFrame = 3 /**< Nothing to write; says nothing about the connection. */
} MqttWriteStatus;

/**
 * @brief The whole of what a client needs from the wire.
 *
 * Implement it to run this client over something else. @c nowMs has no
 * required epoch — only differences are taken from it.
 */
typedef struct MqttTransport {
    /** Writes @p length bytes, reporting ::MqttWriteOk only when all left. */
    MqttWriteStatus (*write)(void* ctx, const uint8_t* bytes, uint32_t length);
    /** Ends the current connection. Reconnecting is the transport's business. */
    void (*close)(void* ctx);
    /** Milliseconds from any fixed origin. */
    uint32_t (*nowMs)(void* ctx);
    /**
     * Advances the connection and delivers any bytes that arrived, through
     * mqttReadAt() and mqttCommitRead(). Must return; may wait up to the
     * client's poll budget.
     */
    void (*poll)(void* ctx, MqttClient* client);
    /** Whether bytes can currently flow. */
    bool (*isConnected)(void* ctx);
    /**
     * Whether the transport has reached a state it will not retry out of —
     * a broker that was never configured, a polling interface that would
     * not open. A stopped transport's @c poll does nothing, so mqttRun()
     * consults this rather than spinning on a poll that has become a
     * no-op.
     *
     * Optional: NULL means the transport never gives up, which is what a
     * transport that only ever backs off and retries should say.
     */
    bool (*isStopped)(void* ctx);
    /**
     * Fills @p out with @p length random bytes, or NULL if the platform has
     * no source. Used once, for a client identifier, when the caller did not
     * supply one — so it needs to differ between two devices off the same
     * production line, not to resist an attacker.
     */
    void (*randomFill)(void* ctx, uint8_t* out, size_t length);
    void* ctx; /**< Passed back to all of the above. */
} MqttTransport;

/**
 * @brief Attaches a transport. Do this before polling.
 * @param client    Client to modify.
 * @param transport How bytes leave and arrive.
 */
void mqttAttach(MqttClient* client, MqttTransport transport);

/**
 * @brief Room for incoming bytes, at the end of what is already held.
 * @param client Client to query.
 * @return Free bytes in the receive buffer.
 */
int mqttReadRoom(MqttClient* client);

/**
 * @brief Where the next read should write.
 *
 * A transport writes straight in here rather than staging bytes of its own,
 * which is what keeps a 2 KB read from being copied twice on a device.
 *
 * @param client Client to query.
 * @return The first free byte of the receive buffer.
 */
uint8_t* mqttReadAt(MqttClient* client);

/**
 * @brief Takes bytes the transport wrote at mqttReadAt().
 * @param client Client to advance.
 * @param count  How many bytes were written.
 * @return true if the stream is still usable. false means it desynced and
 *         the transport should drop the connection.
 */
bool mqttCommitRead(MqttClient* client, int count);

/**
 * @brief Discards a part-received frame, for a connection coming up.
 * @param client Client whose stream restarts.
 */
void mqttResetStream(MqttClient* client);

/**
 * @brief Tells the client its transport has just connected.
 *
 * Starts the keep-alive clocks and sends the CONNECT.
 *
 * @param client Client whose transport came up.
 */
void mqttConnectionUp(MqttClient* client);

/**
 * @brief Reports that the broker accepted the CONNECT, not merely that the
 *        transport connected.
 * @param client Client to query.
 * @return true once a CONNACK has been accepted on the current connection.
 */
bool mqttSessionIsEstablished(MqttClient* client);

/**
 * @brief The identifier this client sends in CONNECT.
 * @param client Client to query.
 * @return Whatever mqttSetClientId() was given, or the generated fallback.
 *         The fallback is only generated once the transport is attached, so
 *         this reads as an empty string before mqttAttach().
 */
const char* mqttClientId(MqttClient* client);

/**
 * @brief The broker hostname the client was configured with.
 * @param client Client to query.
 * @return The hostname, or NULL if none was set.
 */
const char* mqttBrokerHost(MqttClient* client);

/**
 * @brief The broker port the client was configured with.
 * @param client Client to query.
 * @return The port.
 */
uint16_t mqttBrokerPort(MqttClient* client);

/**
 * @brief The poll budget set by mqttSetPollBudget().
 * @param client Client to query.
 * @return Milliseconds a transport's poll may wait.
 */
int mqttPollBudget(MqttClient* client);

#ifdef __cplusplus
}
#endif

#endif
