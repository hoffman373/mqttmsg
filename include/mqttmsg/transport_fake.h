/**
 * @file transport_fake.h
 * @brief A transport backed by arrays and a clock the caller moves.
 *
 * No socket, no network, no waiting. Written bytes land in a buffer a test
 * can read back; frames a test queues are delivered on the next poll; the
 * clock only advances when told to, so keep-alive timing is exact rather
 * than approximate.
 *
 * This is what makes the client itself testable — subscribe, deliver a
 * PUBLISH, assert the right callback ran with the right topic — none of
 * which is reachable through a real transport without a broker.
 *
 * @code
 * MqttFakeTransport* wire = mqttFakeTransportNew();
 * MqttClient* mqtt = mqttNew();
 * mqttFakeTransportAttach(wire, mqtt);
 *
 * mqttFakeConnect(wire);                 // the transport comes up
 * mqttFakeDeliver(wire, connAckFrame);   // the broker answers
 * mqttPoll(mqtt);
 * @endcode
 */

#ifndef MQTTMSG_TRANSPORT_FAKE_H
#define MQTTMSG_TRANSPORT_FAKE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <mqttmsg/mqttmsg.h>

/** @brief A fake transport. Created by mqttFakeTransportNew(). */
typedef struct MqttFakeTransport MqttFakeTransport;

/**
 * @brief Creates a fake transport, disconnected, with its clock at zero.
 * @return The new transport, or NULL if allocation failed.
 */
MqttFakeTransport* mqttFakeTransportNew(void);

/**
 * @brief Releases a fake transport.
 * @param transport Transport to release, or NULL.
 */
void mqttFakeTransportFree(MqttFakeTransport* transport);

/**
 * @brief Attaches the transport to a client.
 * @param transport Transport to attach.
 * @param client    Client to drive.
 */
void mqttFakeTransportAttach(MqttFakeTransport* transport, MqttClient* client);

/**
 * @brief Brings the connection up, as a real transport would on connect.
 *
 * Tells the client, which starts its keep-alive clocks and sends a CONNECT.
 *
 * @param transport Transport to connect.
 */
void mqttFakeConnect(MqttFakeTransport* transport);

/**
 * @brief Queues bytes for the client to receive on its next poll.
 * @param transport Transport to deliver through.
 * @param frame     Bytes to deliver. Copied, so the caller may release it.
 */
void mqttFakeDeliver(MqttFakeTransport* transport, MqttPayload frame);

/**
 * @brief Moves the transport's clock forward.
 * @param transport   Transport to advance.
 * @param milliseconds How far.
 */
void mqttFakeAdvanceClock(MqttFakeTransport* transport, uint32_t milliseconds);

/**
 * @brief How many frames the client has written.
 * @param transport Transport to query.
 * @return The count since creation.
 */
int mqttFakeWriteCount(MqttFakeTransport* transport);

/**
 * @brief The type of one frame the client wrote.
 * @param transport Transport to query.
 * @param index     Which frame, in the order they were written.
 * @return Its message type, or a negative value if @p index is past the
 *         end.
 */
int mqttFakeWrittenType(MqttFakeTransport* transport, int index);

/**
 * @brief One frame the client wrote, for reading its contents back.
 * @param transport Transport to query.
 * @param index     Which frame.
 * @return A view over the recorded bytes, or a None payload if @p index is
 *         past the end. Valid until the transport is freed.
 */
MqttPayload mqttFakeWritten(MqttFakeTransport* transport, int index);

/**
 * @brief How many times the client asked to drop the connection.
 * @param transport Transport to query.
 * @return The count since creation.
 */
int mqttFakeCloseCount(MqttFakeTransport* transport);

/**
 * @brief Makes the next writes fail.
 * @param transport Transport to modify.
 * @param status    What write() should report from now on.
 */
void mqttFakeSetWriteStatus(MqttFakeTransport* transport, MqttWriteStatus status);

/**
 * @brief Puts the transport into the state it will not retry out of.
 *
 * What a real transport does when it finds no broker configured, or cannot
 * open the interface it polls through: it stops, and stays stopped. The
 * connection goes down with it.
 *
 * @param transport Transport to stop.
 */
void mqttFakeStop(MqttFakeTransport* transport);

#ifdef __cplusplus
}
#endif

#endif
