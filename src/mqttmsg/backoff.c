/**
 * @file backoff.c
 * @brief Exponential reconnect pacing, shared by both clients.
 *
 * @see backoff.h
 */

#include "backoff.h"

void backoffInit(ReconnectBackoff* backoff, unsigned minMs, unsigned maxMs, unsigned healthyMs) {
    backoff->minMs = minMs;
    backoff->maxMs = maxMs;
    backoff->healthyMs = healthyMs;
    backoff->currentMs = minMs;
}

unsigned backoffOnAttemptEnded(ReconnectBackoff* backoff, bool sessionEstablished,
                               unsigned sessionDurationMs) {
    if (sessionEstablished && sessionDurationMs >= backoff->healthyMs) {
        backoff->currentMs = backoff->minMs;
    }

    /* The wait returned is the current value; the doubling is for the attempt
       after this one. Growing here rather than at the point of failure is what
       makes the first retry the minimum rather than twice it. */
    unsigned waitMs = backoff->currentMs;

    unsigned next = backoff->currentMs * 2;
    backoff->currentMs = next > backoff->maxMs ? backoff->maxMs : next;

    return waitMs;
}
