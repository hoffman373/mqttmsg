/**
 * @file session.c
 * @brief The transport-independent MQTT session: what to send, when to
 *        send it, and what a broker's answer means.
 *
 * @see session.h
 */

#include "session.h"

#include <stdlib.h>
#include <string.h>

#include <mqttmsg/logger.h>
#include <mqttmsg/mqtt.h>

/** @brief The longest client id MQTT 3.1 requires a broker to accept. */
#define CLIENT_ID_GUARANTEED_MAX 23

void mqttSessionInit(MqttSession* session, MqttTransport transport, uint16_t keepAliveSeconds) {
    session->transport = transport;
    session->keepAliveSeconds = keepAliveSeconds;
    session->lastPingMs = 0;
    session->lastRecvMs = 0;
    session->messageId = 0;
    session->beforePing = NULL;
    session->sessionEstablished = false;
}

void mqttSessionSetKeepAlive(MqttSession* session, uint16_t seconds) {
    session->keepAliveSeconds = seconds;
}

uint32_t mqttSessionPingIntervalMs(const MqttSession* session) {
    /* seconds * 1000 / 2, folded so the division cannot lose a half-second. */
    return (uint32_t)session->keepAliveSeconds * 500u;
}

uint32_t mqttSessionSilenceTimeoutMs(const MqttSession* session) {
    return (uint32_t)session->keepAliveSeconds * 1500u;
}

void mqttSessionConnectionUp(MqttSession* session) {
    uint32_t now = session->transport.nowMs(session->transport.ctx);
    session->lastPingMs = now;
    session->lastRecvMs = now;
    session->sessionEstablished = false;
}

void mqttSessionNoteTraffic(MqttSession* session) {
    session->lastRecvMs = session->transport.nowMs(session->transport.ctx);
}

uint16_t mqttSessionNextMessageId(MqttSession* session) {
    if (++session->messageId == 0) {
        session->messageId = 1;
    }
    return session->messageId;
}

MqttWriteStatus mqttSessionWriteFrame(MqttSession* session, MqttPayload* frame, const char* what) {
    if (!mqttPayloadIsOk(*frame)) {
        mqttmsgErrorPrint("Refusing to send a %s that could not be built.\n", what);
        mqttPayloadFree(frame);
        return MqttWriteNoFrame;
    }

    MqttWriteStatus status = session->transport.write(
        session->transport.ctx, mqttPayloadBytes(*frame), mqttPayloadLength(*frame));

    mqttPayloadFree(frame);
    return status;
}

MqttWriteStatus mqttSessionSendFrame(MqttSession* session, MqttPayload* frame, const char* what) {
    MqttWriteStatus status = mqttSessionWriteFrame(session, frame, what);

    if (status == MqttWriteBusy) {
        mqttmsgDebugPrint("Send queue full, %s dropped but connection kept.\n", what);
    } else if (status == MqttWriteFailed) {
        mqttmsgErrorPrint("Failed to send a %s, forcing reconnect.\n", what);
        session->transport.close(session->transport.ctx);
    }

    return status;
}

MqttWriteStatus mqttSessionSendConnect(MqttSession* session, const char* clientId,
                                       const char* username, const char* password,
                                       const char* willTopic, const char* willMessage,
                                       bool willRetain) {
    if (strlen(clientId) > CLIENT_ID_GUARANTEED_MAX) {
        mqttmsgErrorPrint(
            "Client id is longer than the %d characters MQTT 3.1 guarantees; "
            "the broker may refuse it.\n",
            CLIENT_ID_GUARANTEED_MAX);
    }

    MqttPayload frame = buildConnect(clientId, username, password, willTopic, willMessage,
                                     willRetain, session->keepAliveSeconds);
    return mqttSessionSendFrame(session, &frame, "connect message");
}

bool mqttSessionAcceptConnAck(MqttSession* session, MqttPayload frame) {
    FixedHeader fixedHeader = parseFixedHeader(frame);
    Message parsed = parseMessage(fixedHeader, frame);

    if (parsed.status != MessageOk) {
        mqttmsgErrorPrint("Ignoring a malformed CONNACK.\n");
        return false;
    }

    if (parsed.variableHeader.connReturn != 0) {
        mqttmsgErrorPrint("Broker refused the connection (code %d).\n",
                          parsed.variableHeader.connReturn);
        return false;
    }

    session->sessionEstablished = true;
    return true;
}

MqttWriteStatus mqttSessionSendSubscribe(MqttSession* session, MqttTopicAt topicAt, void* ctx,
                                         int count) {
    if (count <= 0) {
        return MqttWriteNoFrame;
    }

    Subscription* subscriptions = calloc((size_t)count, sizeof(Subscription));
    if (subscriptions == NULL) {
        /* The session stays up but unsubscribed. Saying so beats writing the
           list through a NULL, and the next connect tries again. */
        mqttmsgErrorPrint("Out of memory building the subscription list.\n");
        return MqttWriteNoFrame;
    }

    for (int i = 0; i < count; i++) {
        subscriptions[i].topic = topicAt(ctx, i);
        subscriptions[i].qos = AtMostOnce;
    }

    MqttPayload frame = buildSubscribe(mqttSessionNextMessageId(session), subscriptions, count);
    free(subscriptions);

    return mqttSessionSendFrame(session, &frame, "subscribe message");
}

MqttWriteStatus mqttSessionPublishString(MqttSession* session, const char* topic,
                                         const char* message, bool retain) {
    MqttPayload body = makeStringPayload(message);
    MqttPayload frame = buildPublish(topic, mqttSessionNextMessageId(session), body, retain);
    mqttPayloadFree(&body);

    MQTTMSG_DUMP_PAYLOAD_ASCII(frame);

    return mqttSessionSendFrame(session, &frame, "publish message");
}

MqttWriteStatus mqttSessionSendPing(MqttSession* session) {
    if (session->beforePing != NULL) {
        session->beforePing(session->transport.ctx);
    }

    MqttPayload frame = buildPingReq();
    mqttmsgDebugPrint("Sending ping req.\n");
    return mqttSessionSendFrame(session, &frame, "ping request");
}

MqttWriteStatus mqttSessionSendDisconnect(MqttSession* session) {
    MqttPayload frame = buildDisconnectMsg();
    mqttmsgDebugPrint("Sending disconnect.\n");
    return mqttSessionWriteFrame(session, &frame, "disconnect message");
}

MqttKeepAliveAction mqttSessionServiceKeepAlive(MqttSession* session) {
    if (session->keepAliveSeconds == 0) {
        return MqttKeepAliveIdle;
    }

    uint32_t now = session->transport.nowMs(session->transport.ctx);

    if (now - session->lastRecvMs > mqttSessionSilenceTimeoutMs(session)) {
        mqttmsgErrorPrint("No traffic from broker within keep-alive window, forcing reconnect.\n");
        /* Sent while the connection is still writable, so the broker drops the
           will rather than publishing it on what looks to it like a crash. */
        mqttSessionSendDisconnect(session);
        session->transport.close(session->transport.ctx);
        return MqttKeepAliveTimedOut;
    }

    if (now - session->lastPingMs > mqttSessionPingIntervalMs(session)) {
        mqttSessionSendPing(session);
        session->lastPingMs = now;
        return MqttKeepAlivePinged;
    }

    return MqttKeepAliveIdle;
}

void mqttBuildClientId(char* dest, size_t size, MqttRandomFill fill, void* ctx) {
    if (dest == NULL || size < 1) {
        return;
    }

    size_t length = size - 1;
    fill(ctx, (uint8_t*)dest, length);
    for (size_t i = 0; i < length; i++) {
        dest[i] = (char)((unsigned char)dest[i] % 26 + 'A');
    }

    dest[length] = '\0';
}
