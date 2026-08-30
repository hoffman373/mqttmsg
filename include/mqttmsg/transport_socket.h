/**
 * @file transport_socket.h
 * @brief A BSD socket transport for the MQTT client.
 *
 * The host counterpart to transport_pico.h. Resolves the broker, opens a
 * TCP connection, reopens it when it drops — paced by the same exponential
 * backoff the device uses — and hands received bytes to the client.
 *
 * @code
 * MqttSocketTransport* wire = mqttSocketTransportNew();
 * MqttClient* mqtt = mqttNew();
 * mqttSocketTransportAttach(wire, mqtt);
 * @endcode
 *
 * @note Host-only; needs `epoll`, so Linux rather than any POSIX.
 */

#ifndef MQTTMSG_TRANSPORT_SOCKET_H
#define MQTTMSG_TRANSPORT_SOCKET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <mqttmsg/mqttmsg.h>

/** @brief A socket transport. Created by mqttSocketTransportNew(). */
typedef struct MqttSocketTransport MqttSocketTransport;

/**
 * @brief Creates a socket transport, not yet connected to anything.
 * @return The new transport, or NULL if it could not be created — either
 *         the allocation or the polling interface it holds for its life.
 */
MqttSocketTransport* mqttSocketTransportNew(void);

/**
 * @brief Releases a transport and the descriptors it holds.
 * @param transport Transport to release, or NULL.
 */
void mqttSocketTransportFree(MqttSocketTransport* transport);

/**
 * @brief Attaches the transport to a client.
 *
 * Both directions at once: the client sends through the transport, and the
 * transport delivers received bytes to the client.
 *
 * @param transport Transport to attach.
 * @param client    Client to drive. Must outlive the transport.
 */
void mqttSocketTransportAttach(MqttSocketTransport* transport, MqttClient* client);

#ifdef __cplusplus
}
#endif

#endif
