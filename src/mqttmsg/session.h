/**
 * @file session.h
 * @internal
 * @brief The protocol half of an MQTT client, with no transport in it.
 *
 * What both clients do once bytes can flow: send the CONNECT, read the
 * broker's answer out of a CONNACK, send the SUBSCRIBE, publish, pace
 * keep-alive pings and notice a connection that has gone quiet. None of
 * that depends on how the bytes leave the machine, so none of it is
 * written twice.
 *
 * The transport supplies three things — write, close and "now" — through
 * ::MqttTransport. lwIP is one implementation of that and a BSD socket is
 * another; an array and a counter is a third, which is what makes the
 * sequencing here reachable from a host test.
 *
 * Internal to the library; not shipped in `include/`.
 *
 * @see framing.h for the receive-side counterpart.
 */

#ifndef MQTTMSG_SESSION_H
#define MQTTMSG_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <mqttmsg/mqtt_types.h>
#include <mqttmsg/mqttmsg.h>

/*
 * ::MqttWriteStatus and ::MqttTransport are public, in mqttmsg.h, because a
 * transport is the documented extension point. The session uses three of
 * the six members — write, close and nowMs — and never the ones about
 * driving a connection, which are the client's business.
 */

/** @internal @brief Protocol state for one client, across reconnects. */
typedef struct MqttSession {
    MqttTransport transport; /**< The bytes-out half. */
    /**
     * The keep-alive period, in seconds. One number with three consumers:
     * it goes out in the CONNECT, the PINGREQ pacing is half of it, and the
     * silence watchdog is the one-and-a-half times the protocol allows a
     * broker before it hangs up. Zero turns all three off, which is what
     * MQTT reads a zero in the CONNECT as.
     *
     * Set it through mqttSessionSetKeepAlive() rather than by hand, so the
     * derived intervals below cannot be reached without it.
     */
    uint16_t keepAliveSeconds;
    uint32_t lastPingMs; /**< @c nowMs of the last PINGREQ sent. */
    uint32_t lastRecvMs; /**< @c nowMs of the last byte received. */
    uint16_t messageId;  /**< Rolling message id counter. */
    /**
     * Called with MqttTransport::ctx just before each keep-alive PINGREQ,
     * or NULL. The hook a client needs when something of its own has to
     * ride along with the ping.
     */
    void (*beforePing)(void* ctx);
    /**
     * Set once the broker has accepted a CONNECT. Distinguishes reaching a
     * working broker from completing a handshake, which is the distinction
     * reconnect pacing resets on.
     */
    bool sessionEstablished;
} MqttSession;

/**
 * @internal
 * @brief Fills a session with a transport and a keep-alive period.
 * @param session          Session to initialise. Every field is written.
 * @param transport        How bytes leave, how the connection ends, and
 *                         what time it is.
 * @param keepAliveSeconds Keep-alive period; 0 for none.
 */
void mqttSessionInit(MqttSession* session, MqttTransport transport, uint16_t keepAliveSeconds);

/**
 * @internal
 * @brief Sets the keep-alive period.
 *
 * Takes effect on the next CONNECT, which is where the broker learns the
 * number; the pacing derived from it changes immediately.
 *
 * @param session Session to modify.
 * @param seconds Keep-alive period, or 0 to turn keep-alive off.
 */
void mqttSessionSetKeepAlive(MqttSession* session, uint16_t seconds);

/**
 * @internal
 * @brief How long between PINGREQs.
 * @param session Session to query.
 * @return Half the keep-alive period, in milliseconds. Half rather than all
 *         of it because the broker is entitled to hang up at one and a half
 *         times, and a ping that arrives at exactly the deadline has
 *         already lost the race to jitter.
 */
uint32_t mqttSessionPingIntervalMs(const MqttSession* session);

/**
 * @internal
 * @brief How long the broker may say nothing before it counts as dead.
 * @param session Session to query.
 * @return One and a half keep-alive periods, in milliseconds — the same
 *         bound the protocol gives the broker over this client.
 */
uint32_t mqttSessionSilenceTimeoutMs(const MqttSession* session);

/**
 * @internal
 * @brief Prepares the session for a connection that has just come up.
 *
 * Starts both clocks now, so a fresh connection gets a full keep-alive
 * window to prove itself rather than inheriting the last one's silence.
 *
 * @param session Session the connection belongs to.
 */
void mqttSessionConnectionUp(MqttSession* session);

/**
 * @internal
 * @brief Records that bytes arrived, whatever they were.
 *
 * Any traffic at all proves the connection is alive, which is what the
 * silence watchdog in mqttSessionServiceKeepAlive() reads.
 *
 * @param session Session the bytes arrived on.
 */
void mqttSessionNoteTraffic(MqttSession* session);

/**
 * @internal
 * @brief Allocates the next message identifier.
 * @param session Session to draw from.
 * @return A non-zero identifier. Zero is skipped on both the initial
 *         counter and every 16-bit wrap, because a zero id is dropped from
 *         the serialised frame while QoS 1 is still declared, which
 *         desyncs the broker.
 */
uint16_t mqttSessionNextMessageId(MqttSession* session);

/**
 * @internal
 * @brief Writes a built frame to the transport, then releases it.
 *
 * Every send builds a frame, writes it and frees it, and every one of them
 * has to answer the same question first: whether there is a frame at all.
 * A builder that ran out of memory hands back a failure whose reason sits
 * where a length would, so writing without asking would pass a NULL
 * pointer and a length of four billion.
 *
 * @param session Session to write on.
 * @param frame   Frame to send; released and left None whatever happens.
 * @param what    Message type, for the error line.
 * @return What the transport reported, or ::MqttWriteNoFrame if @p frame
 *         could not be built.
 * @note Leaves the connection alone on failure. mqttSessionSendFrame() is
 *       the one that acts on it.
 */
MqttWriteStatus mqttSessionWriteFrame(MqttSession* session, MqttPayload* frame, const char* what);

/**
 * @internal
 * @brief mqttSessionWriteFrame(), tearing the connection down if the write
 *        failed for a reason that will not clear on its own.
 *
 * A publish that never left the device looks, from anywhere else, exactly
 * like one that arrived. The keep-alive watchdog does eventually notice a
 * connection this broken, but only after a full window of silence — and a
 * refused write is the earlier, cheaper signal that something is wrong.
 *
 * ::MqttWriteBusy and ::MqttWriteNoFrame are the exceptions: dropping a
 * working session over back-pressure would be worse than the message that
 * was lost, and a frame that was never built says nothing about the
 * connection at all.
 *
 * @param session Session to write on.
 * @param frame   Frame to send; released whatever happens.
 * @param what    Message type, for the error line.
 * @return What the write reported.
 */
MqttWriteStatus mqttSessionSendFrame(MqttSession* session, MqttPayload* frame, const char* what);

/**
 * @internal
 * @brief Sends the CONNECT that opens a session.
 *
 * Advertises MqttSession::keepAliveSeconds, so the number the broker times
 * this client against is the same one the pings are paced from.
 *
 * @param session     Session to connect.
 * @param clientId    MQTT client identifier. Longer than 23 characters is
 *                    logged and sent anyway; the guarantee MQTT 3.1 gives
 *                    stops there, but brokers routinely accept more.
 * @param username    CONNECT username, or NULL.
 * @param password    CONNECT password, or NULL.
 * @param willTopic   Last-will topic, or NULL for no will.
 * @param willMessage Last-will payload.
 * @param willRetain  Whether the broker retains the will.
 * @return What the write reported.
 */
MqttWriteStatus mqttSessionSendConnect(MqttSession* session, const char* clientId,
                                       const char* username, const char* password,
                                       const char* willTopic, const char* willMessage,
                                       bool willRetain);

/**
 * @internal
 * @brief Reads the broker's verdict out of a CONNACK and records it.
 *
 * On acceptance MqttSession::sessionEstablished is set, which is what a
 * transport's reconnect pacing keys its reset off.
 *
 * @param session Session the frame arrived on.
 * @param frame   The whole CONNACK, header included.
 * @return true if the broker accepted the connection.
 */
bool mqttSessionAcceptConnAck(MqttSession* session, MqttPayload frame);

/**
 * @internal
 * @brief Where a SUBSCRIBE gets its topic filters from.
 * @param ctx   The context pointer handed to mqttSessionSendSubscribe().
 * @param index Which filter is wanted, counting from zero.
 * @return The filter, NUL-terminated.
 */
typedef const char* (*MqttTopicAt)(void* ctx, int index);

/**
 * @internal
 * @brief Sends one SUBSCRIBE covering @p count topic filters, all at QoS 0.
 *
 * Subscriptions do not survive a session, so this belongs on every connect
 * rather than only the first.
 *
 * @param session Session to subscribe on.
 * @param topicAt Supplies the filters, called once per index.
 * @param ctx     Passed back to @p topicAt.
 * @param count   How many filters there are. Zero or fewer sends nothing.
 * @return What the write reported, or ::MqttWriteNoFrame if there was
 *         nothing to subscribe to or the filter list could not be
 *         allocated.
 */
MqttWriteStatus mqttSessionSendSubscribe(MqttSession* session, MqttTopicAt topicAt, void* ctx,
                                         int count);

/**
 * @internal
 * @brief Publishes a NUL-terminated string.
 * @param session Session to publish on.
 * @param topic   Topic to publish to.
 * @param message Message body.
 * @param retain  Whether the broker retains the message.
 * @return What the write reported.
 */
MqttWriteStatus mqttSessionPublishString(MqttSession* session, const char* topic,
                                         const char* message, bool retain);

/**
 * @internal
 * @brief Sends a keep-alive PINGREQ.
 * @param session Session to ping on.
 * @return What the write reported.
 */
MqttWriteStatus mqttSessionSendPing(MqttSession* session);

/**
 * @internal
 * @brief Sends a clean DISCONNECT, telling the broker to drop the will
 *        without publishing it.
 *
 * @param session Session to disconnect.
 * @return What the write reported.
 * @warning Must be sent while the connection is still writable, before any
 *          close this client initiates. Otherwise the broker treats the
 *          close as an abnormal drop and fires the possibly-retained will,
 *          even though nothing is wrong.
 * @note Unlike the other sends, a failure here does not close the
 *       connection: the caller is closing it anyway, and a second close
 *       would count as a second finished attempt against the transport's
 *       reconnect pacing.
 */
MqttWriteStatus mqttSessionSendDisconnect(MqttSession* session);

/** @internal @brief What mqttSessionServiceKeepAlive() did. */
typedef enum {
    /** Nothing was due. */
    MqttKeepAliveIdle = 0,
    /** The interval elapsed and a PINGREQ went out. */
    MqttKeepAlivePinged = 1,
    /**
     * The broker said nothing for longer than the watchdog allows. A
     * DISCONNECT was attempted and the connection was closed.
     */
    MqttKeepAliveTimedOut = 2
} MqttKeepAliveAction;

/**
 * @internal
 * @brief Sends a ping if one is due, and drops the connection if the
 *        broker has gone silent.
 *
 * Silence past mqttSessionSilenceTimeoutMs() means the connection is dead
 * even though nothing said so — no FIN, no RST, the way a NAT or conntrack
 * entry expires. Waiting for an answer that will never come is the failure
 * this catches.
 *
 * @param session Session to service. Does nothing if keep-alive is off.
 * @see mqttSessionPingIntervalMs, mqttSessionSilenceTimeoutMs
 * @return Which of the two, if either, happened.
 */
MqttKeepAliveAction mqttSessionServiceKeepAlive(MqttSession* session);

/**
 * @internal
 * @brief Supplies random bytes to mqttBuildClientId().
 * @param ctx    The context pointer it was called with.
 * @param out    Where to write @p length bytes.
 * @param length How many bytes are wanted.
 */
typedef void (*MqttRandomFill)(void* ctx, uint8_t* out, size_t length);

/**
 * @internal
 * @brief Fills @p dest with a random NUL-terminated client identifier.
 *
 * @p size is the buffer size, not the identifier length: the result is
 * @p size - 1 random uppercase letters and a terminator, because every
 * caller hands it to buildConnect(), which calls `strlen()` on it.
 *
 * @param dest Destination. Does nothing if NULL or if @p size is below 1.
 * @param size Bytes available in @p dest, terminator included.
 * @param fill Random source.
 * @param ctx  Passed back to @p fill.
 */
void mqttBuildClientId(char* dest, size_t size, MqttRandomFill fill, void* ctx);

#ifdef __cplusplus
}
#endif

#endif
