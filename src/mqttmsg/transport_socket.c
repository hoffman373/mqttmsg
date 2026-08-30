/**
 * @file transport_socket.c
 * @brief BSD sockets and `epoll` under the MQTT client.
 *
 * Owns a connection and the pacing of getting one back: resolve, connect,
 * read, and a reconnect deadline the caller steps over rather than sleeps
 * through. It knows nothing about MQTT beyond handing bytes up and taking
 * bytes down.
 *
 * @see transport_socket.h
 */

#include <mqttmsg/transport_socket.h>

#include <mqttmsg/logger.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "backoff.h"

/*
 * Reconnect pacing. A broker that is down is down for minutes, not
 * milliseconds, so retrying it as fast as the kernel will refuse the socket
 * achieves nothing except a hot core and a log nobody can read. The policy
 * itself lives in backoff.h and is shared with the Pico transport.
 */
/** @brief Shortest wait between connection attempts. */
#define RECONNECT_BACKOFF_MIN_MS 1000u
/** @brief Longest wait between connection attempts. */
#define RECONNECT_BACKOFF_MAX_MS 120000u
/** @brief How long a session must last to count as healthy. */
#define SESSION_HEALTHY_MS 30000u

/** @brief The structure behind MqttSocketTransport. */
struct MqttSocketTransport {
    MqttClient* client;       /**< The client being driven. */
    int socket_desc;          /**< Connected socket, or -1. */
    int pollFd;               /**< epoll instance watching @c socket_desc. */
    ReconnectBackoff backoff; /**< Reconnect pacing, charged once per attempt. */
    /**
     * nowMs() before which no attempt starts. This is what the reconnect
     * wait is: a deadline the caller polls past, rather than a sleep that
     * held the thread for up to two minutes.
     */
    uint32_t retryAfterMs;
    bool isConnected;   /**< A socket is up and the CONNECT has gone out. */
    bool stopped;       /**< An unrecoverable error ended the transport. */
    bool dropRequested; /**< The client asked for the connection to end. */
    time_t connectedAt; /**< When the current attempt connected, or 0. */
};

/**
 * @brief Milliseconds from an arbitrary origin.
 *
 * Monotonic, so it survives a wall-clock step that would otherwise skip or
 * stall a ping.
 *
 * @return The reading. Wraps every 49.7 days, which every comparison here
 *         is written to survive.
 */
static uint32_t nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000));
}

/* ── The MqttTransport interface ───────────────────────────────────── */

/**
 * @brief Writes bytes to the connected socket.
 *
 * `send()` is free to take fewer bytes than it was offered, so a frame that
 * goes out in pieces is finished here rather than reported as sent.
 *
 * `MSG_NOSIGNAL` keeps a write to a peer that has already closed from
 * raising `SIGPIPE`, whose default disposition would take the whole process
 * down. With it the write fails with `EPIPE` instead, which is the error the
 * reconnect path below is written to handle.
 *
 * @param ctx    The transport, as a `void*`.
 * @param bytes  Bytes to send.
 * @param length How many bytes to send.
 * @return ::MqttWriteOk once every byte has left, ::MqttWriteFailed
 *         otherwise. A socket has no back-pressure state that clears on its
 *         own, so ::MqttWriteBusy never comes back from here.
 */
static MqttWriteStatus socketWrite(void* ctx, const uint8_t* bytes, uint32_t length) {
    MqttSocketTransport* t = (MqttSocketTransport*)ctx;

    uint32_t sent = 0;
    while (sent < length) {
        ssize_t n = send(t->socket_desc, bytes + sent, length - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return MqttWriteFailed;
        }
        sent += (uint32_t)n;
    }

    return MqttWriteOk;
}

/**
 * @brief Ends the current connection at the client's request.
 *
 * The socket is closed at the end of the attempt rather than here, because
 * that is also where the backoff is charged, and doing one without the
 * other would leave the pacing wrong.
 *
 * @param ctx The transport, as a `void*`.
 */
static void socketClose(void* ctx) { ((MqttSocketTransport*)ctx)->dropRequested = true; }

/**
 * @brief The transport's clock.
 * @param ctx Unused; the clock is the machine's.
 * @return Milliseconds from an arbitrary origin.
 */
static uint32_t socketNowMs(void* ctx) {
    (void)ctx;
    return nowMs();
}

/**
 * @brief Whether bytes can currently flow.
 * @param ctx The transport, as a `void*`.
 * @return true while a connection is up.
 */
static bool socketIsConnected(void* ctx) { return ((MqttSocketTransport*)ctx)->isConnected; }

/**
 * @brief Random bytes from the kernel.
 * @param ctx    Unused; the pool is the machine's.
 * @param out    Where to write @p length bytes.
 * @param length How many bytes are wanted.
 */
static void socketRandomFill(void* ctx, uint8_t* out, size_t length) {
    (void)ctx;
    /* Short reads only happen on a signal with a length above 256; a client
       identifier is far below that, so a partial fill would cost a character
       of entropy rather than break anything. */
    (void)!getrandom(out, length, 0);
}

/* ── Attempts ──────────────────────────────────────────────────────── */

/**
 * @brief Charges the backoff for a finished attempt and arms the next one.
 *
 * Every way an attempt can end comes through here — a failed resolve, a
 * refused socket, a rejected CONNECT, a dropped session — so the pacing and
 * the doubling live in one place rather than beside each failure. Only a
 * session that both reached a working broker and lasted earns the wait back
 * down; everything else leaves it growing.
 *
 * @param t Transport whose attempt ended.
 */
static void endAttempt(MqttSocketTransport* t) {
    unsigned sessionMs = t->connectedAt == 0 ? 0 : (unsigned)(time(NULL) - t->connectedAt) * 1000u;
    unsigned waitMs =
        backoffOnAttemptEnded(&t->backoff, mqttSessionIsEstablished(t->client), sessionMs);

    t->retryAfterMs = nowMs() + waitMs;
    mqttmsgDebugPrint("Reconnecting in %us.\n", waitMs / 1000);

    if (t->socket_desc >= 0) {
        if (epoll_ctl(t->pollFd, EPOLL_CTL_DEL, t->socket_desc, NULL) < 0) {
            mqttmsgErrorPrint("Failed to unregister the MQTT socket with epoll.\n");
        }
        close(t->socket_desc);
        t->socket_desc = -1;
    }

    t->isConnected = false;
    t->dropRequested = false;
    t->connectedAt = 0;
}

/* -Wanalyzer-fd-leak fires on the connect() branch below. The analyzer
   cannot follow socket_desc — a field of a heap struct — into endAttempt(),
   which closes it. Every way out of this function is covered: the resolve
   failure happens before the socket exists, the epoll registration failure
   closes what it opened, the connect failure hands over to endAttempt(),
   and the success path deliberately leaves the socket open for the poll
   loop to use. Suppressed rather than restructured, because the alternative
   is closing eagerly here and risking a double close. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
/**
 * @brief Resolves the broker, opens a socket and tells the client it is up.
 *
 * The resolve and the TCP handshake are still blocking calls. On a local
 * network they are milliseconds; on a sick resolver or against a host that
 * drops the SYN they are not, and this is the one place a poll can hold the
 * thread for longer than its budget.
 *
 * @param t Transport to connect.
 */
static void beginAttempt(MqttSocketTransport* t) {
    const char* host = mqttBrokerHost(t->client);
    if (host == NULL) {
        mqttmsgErrorPrint("No broker configured; call mqttSetBroker() before polling.\n");
        t->stopped = true;
        return;
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int gai_err = getaddrinfo(host, NULL, &hints, &res);
    if (gai_err != 0 || !res) {
        mqttmsgErrorPrint("Failed to resolve host '%s': %s\n", host, gai_strerror(gai_err));
        endAttempt(t);
        return;
    }

    struct sockaddr_in server;
    memcpy(&server, res->ai_addr, sizeof(struct sockaddr_in));
    server.sin_port = htons(mqttBrokerPort(t->client));
    freeaddrinfo(res);

    t->socket_desc = socket(AF_INET, SOCK_STREAM, 0);
    if (t->socket_desc < 0) {
        mqttmsgErrorPrint("Could not create socket.\n");
        endAttempt(t);
        return;
    }

    int keepalive = 1;
    setsockopt(t->socket_desc, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(int));

    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.data.fd = t->socket_desc;
    event.events = EPOLLIN;
    if (epoll_ctl(t->pollFd, EPOLL_CTL_ADD, t->socket_desc, &event) < 0) {
        /* Nothing about this improves by retrying, and without the socket
           registered the poll below would never see a readable byte. */
        mqttmsgErrorPrint("Failed to register the MQTT socket with epoll.\n");
        close(t->socket_desc);
        t->socket_desc = -1;
        t->stopped = true;
        return;
    }

    if (connect(t->socket_desc, (struct sockaddr*)&server, sizeof(server)) < 0) {
        mqttmsgErrorPrint("Failed to connect.\n");
        endAttempt(t);
        return;
    }

    t->isConnected = true;
    t->connectedAt = time(NULL);
    mqttmsgDebugPrint("connected\n");

    mqttConnectionUp(t->client);
}
#pragma GCC diagnostic pop

/**
 * @brief Waits up to the client's poll budget for bytes, then delivers them.
 * @param t Transport to service. Must be connected.
 */
static void serviceConnection(MqttSocketTransport* t) {
    struct epoll_event events[5];
    int n = epoll_wait(t->pollFd, events, 5, mqttPollBudget(t->client));
    if (n <= 0) {
        return;
    }

    /* Read into the space after whatever is already held, so a frame that
       arrived in pieces is completed rather than overwritten. */
    int room = mqttReadRoom(t->client);
    if (room <= 0) {
        /* A frame larger than the buffer can never complete, so waiting for
           more of it would block forever. */
        mqttmsgErrorPrint("Receive buffer full with no complete message, reconnecting.\n");
        t->isConnected = false;
        return;
    }

    int recvd = recv(t->socket_desc, mqttReadAt(t->client), (size_t)room, 0);
    if (recvd < 0) {
        mqttmsgErrorPrint("recv failed, attempting to reconnect\n");
        t->isConnected = false;
    } else if (recvd == 0) {
        mqttmsgDebugPrint("Received 0 bytes, connection dropped.\n");
        t->isConnected = false;
    } else if (!mqttCommitRead(t->client, recvd)) {
        /* Remaining-length that never terminated: the stream is desynced,
           not merely short, and no amount of further reading resyncs it. */
        mqttmsgErrorPrint("Malformed MQTT fixed header, reconnecting.\n");
        t->isConnected = false;
    }
}

/**
 * @brief Whether the transport has reached a state it will not retry out of.
 * @param ctx The transport, as a `void*`.
 * @return true once an unrecoverable error has ended it — no broker
 *         configured, or an epoll registration that failed.
 */
static bool socketIsStopped(void* ctx) { return ((MqttSocketTransport*)ctx)->stopped; }

/**
 * @brief Advances the connection by one step.
 * @param ctx    The transport, as a `void*`.
 * @param client The client being driven; the same one it was attached to.
 */
static void socketPoll(void* ctx, MqttClient* client) {
    MqttSocketTransport* t = (MqttSocketTransport*)ctx;
    (void)client;

    if (t->stopped) {
        return;
    }

    if (!t->isConnected) {
        /* Signed difference, so the comparison survives the millisecond
           clock wrapping. */
        if ((int32_t)(nowMs() - t->retryAfterMs) < 0) {
            return;
        }

        beginAttempt(t);
        return;
    }

    serviceConnection(t);

    if (!t->isConnected || t->dropRequested) {
        endAttempt(t);
    }
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

MqttSocketTransport* mqttSocketTransportNew(void) {
    MqttSocketTransport* t = calloc(1, sizeof(MqttSocketTransport));
    if (t == NULL) {
        return NULL;
    }

    t->pollFd = epoll_create1(0);
    if (t->pollFd < 0) {
        mqttmsgErrorPrint("Failed to create epoll interface.\n");
        free(t);
        return NULL;
    }

    /* calloc would leave this 0, which is stdin — a descriptor endAttempt()
       would close on the first failed connection. */
    t->socket_desc = -1;
    backoffInit(&t->backoff, RECONNECT_BACKOFF_MIN_MS, RECONNECT_BACKOFF_MAX_MS,
                SESSION_HEALTHY_MS);
    /* Zero is already in the past, so the first attempt starts immediately. */
    t->retryAfterMs = 0;

    return t;
}

void mqttSocketTransportFree(MqttSocketTransport* t) {
    if (t == NULL) {
        return;
    }

    if (t->socket_desc >= 0) {
        close(t->socket_desc);
    }
    if (t->pollFd >= 0) {
        close(t->pollFd);
    }

    free(t);
}

void mqttSocketTransportAttach(MqttSocketTransport* t, MqttClient* client) {
    t->client = client;

    MqttTransport iface = {
        .write = socketWrite,
        .close = socketClose,
        .nowMs = socketNowMs,
        .poll = socketPoll,
        .isConnected = socketIsConnected,
        .isStopped = socketIsStopped,
        .randomFill = socketRandomFill,
        .ctx = t,
    };
    mqttAttach(client, iface);
}
