/**
 * @file transport_pico.h
 * @brief An lwIP transport for the MQTT client, on a Pico W.
 *
 * The device counterpart to transport_socket.h. Brings up the CYW43 link,
 * resolves the broker, opens a TCP connection, reopens it when it drops —
 * paced by the same exponential backoff the host uses — and hands received
 * bytes to the client.
 *
 * @code
 * MqttPicoTransport* wire = mqttPicoTransportNew();
 * mqttPicoSetWifi(wire, WIFI_SSID, WIFI_PASSWORD);
 *
 * MqttClient* mqtt = mqttNew();
 * mqttPicoTransportAttach(wire, mqtt);
 * @endcode
 *
 * @note Pico-only; requires lwIP and the Pico SDK. This is the one header
 *       an otherwise portable application includes for its platform.
 */

#ifndef MQTTMSG_TRANSPORT_PICO_H
#define MQTTMSG_TRANSPORT_PICO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <mqttmsg/mqttmsg.h>

/**
 * @brief Where the transport is in bringing up a connection.
 *
 * The progression is linear from ::MqttPicoDisconnected to
 * ::MqttPicoConnected; any failure returns to ::MqttPicoDisconnected, where
 * the backoff decides when the next attempt starts.
 */
typedef enum {
    MqttPicoDisconnected,  /**< Nothing is up. Waiting out the reconnect backoff. */
    MqttPicoAssociating,   /**< Associating with the WiFi network. */
    MqttPicoLinkUp,        /**< Link is up and an address has been assigned. */
    MqttPicoResolving,     /**< Broker hostname lookup is in flight. */
    MqttPicoResolved,      /**< Broker address resolved. */
    MqttPicoResolveFailed, /**< Lookup failed; the attempt is abandoned. */
    MqttPicoConnecting,    /**< TCP handshake in progress. */
    MqttPicoConnected      /**< TCP is up and bytes can flow. */
} MqttPicoState;

/** @brief An lwIP transport. Created by mqttPicoTransportNew(). */
typedef struct MqttPicoTransport MqttPicoTransport;

/**
 * @brief Creates an lwIP transport, not yet associated with anything.
 * @return The new transport, or NULL if allocation failed.
 */
MqttPicoTransport* mqttPicoTransportNew(void);

/**
 * @brief Releases a transport.
 * @param transport Transport to release, or NULL.
 */
void mqttPicoTransportFree(MqttPicoTransport* transport);

/**
 * @brief Attaches the transport to a client.
 * @param transport Transport to attach.
 * @param client    Client to drive. Must outlive the transport.
 */
void mqttPicoTransportAttach(MqttPicoTransport* transport, MqttClient* client);

/**
 * @brief Sets the WiFi network to associate with.
 *
 * The one piece of configuration with no host equivalent, which is why it
 * lives here rather than in mqttmsg.h.
 *
 * @param transport Transport to modify.
 * @param ssid      NUL-terminated SSID. Stored by pointer.
 * @param password  NUL-terminated passphrase. Stored by pointer.
 */
void mqttPicoSetWifi(MqttPicoTransport* transport, const char* ssid, const char* password);

/**
 * @brief How far the transport has got in bringing a connection up.
 * @param transport Transport to query.
 * @return Where it is.
 */
MqttPicoState mqttPicoConnectionState(MqttPicoTransport* transport);

#ifdef __cplusplus
}
#endif

#endif
