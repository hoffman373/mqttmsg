/**
 * @file backoff.h
 * @internal
 * @brief How long to wait before trying a broker again, shared by both
 *        clients.
 *
 * The rule is one line: double the wait after every attempt that did not
 * produce a healthy session, and go back to the minimum after one that did.
 * Everything interesting is in what counts as healthy, which is why it
 * lives in one tested place rather than being reimplemented per transport.
 *
 * Internal to the library; not shipped in `include/`.
 */

#ifndef MQTTMSG_BACKOFF_H
#define MQTTMSG_BACKOFF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/** @internal @brief Reconnect pacing state for one client. */
typedef struct {
    unsigned currentMs; /**< Wait the next finished attempt will return. */
    unsigned minMs;     /**< Floor, and the value a healthy session resets to. */
    unsigned maxMs;     /**< Ceiling the doubling saturates at. */
    /**
     * A session must last at least this long, on top of completing the MQTT
     * handshake, before the wait resets. Without the duration test a broker
     * that accepts a connection and drops it again resets the backoff every
     * cycle and is never actually paced.
     */
    unsigned healthyMs;
} ReconnectBackoff;

/**
 * @internal
 * @brief Initialises pacing state, armed at @p minMs.
 * @param backoff   State to initialise.
 * @param minMs     Shortest wait, and what a healthy session resets to.
 * @param maxMs     Longest wait.
 * @param healthyMs How long an established session must last to count as
 *                  healthy.
 */
void backoffInit(ReconnectBackoff* backoff, unsigned minMs, unsigned maxMs, unsigned healthyMs);

/**
 * @internal
 * @brief Records a finished connection attempt and yields the next wait.
 *
 * Call once per finished attempt — refused socket, failed lookup, rejected
 * CONNECT, dropped session, all of them.
 *
 * @param backoff            Pacing state to advance.
 * @param sessionEstablished Whether the broker accepted the CONNECT, not
 *                           merely that TCP connected.
 * @param sessionDurationMs  How long the session lasted, measured from the
 *                           transport connecting. Ignored when
 *                           @p sessionEstablished is false.
 * @return Milliseconds to wait before the next attempt. The wait returned
 *         is the current value and the doubling is left behind for the
 *         attempt after, which is what makes the first retry the minimum
 *         rather than twice it.
 */
unsigned backoffOnAttemptEnded(ReconnectBackoff* backoff, bool sessionEstablished,
                               unsigned sessionDurationMs);

#ifdef __cplusplus
}
#endif

#endif
