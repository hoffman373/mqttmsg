/**
 * @file transport_pico.c
 * @brief WiFi association, DNS and TCP connection management, under the
 *        MQTT client.
 *
 * A state machine advanced one step per advanceConnection() call, with an
 * exponential backoff between attempts so a broker that is down does not
 * cost a reassociation, a lookup and a connect on every tick.
 *
 * @see transport_pico.h
 */

#include <mqttmsg/transport_pico.h>
#include <mqttmsg/logger.h>
#include "hardware/regs/addressmap.h"
#include "hardware/regs/rosc.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "pico/cyw43_arch.h"
#include <pico/time.h>

#include "backoff.h"

/** @brief How often lwIP polls an idle connection, in seconds. */
#define POLL_TIME_S 5
/** @brief How long one connection attempt may take before it is abandoned. */
#define CONNECTION_TIMEOUT_MS 20000

/*
 * Reconnect pacing, identical to the host client's and using the same
 * shared policy. Without it a broker that is down means a WiFi
 * reassociation, a DNS lookup and a TCP connect every run-loop tick — 10 ms
 * in most consumers — competing with the loop that feeds the watchdog.
 */
/** @brief Shortest wait between connection attempts. */
#define RECONNECT_BACKOFF_MIN_MS 1000u
/** @brief Longest wait between connection attempts. */
#define RECONNECT_BACKOFF_MAX_MS 120000u
/** @brief How long a session must last to count as healthy. */
#define SESSION_HEALTHY_MS 30000u

/** @brief The connection structure behind MqttPicoTransport. */
struct MqttPicoTransport {
    MqttClient *client;       /**< The client being driven. */
    MqttPicoState state;      /**< Where the machine has got to. */
    ip_addr_t server_address; /**< Resolved broker address. */
    struct tcp_pcb *tcp_pcb;  /**< lwIP protocol control block. */
    const char *wifiSSID;     /**< Network to associate with. */
    const char *wifiPass;     /**< Network passphrase. */
    ReconnectBackoff backoff; /**< Pacing state between attempts. */
    /**
     * Deadline for the attempt currently in flight. Set when the association
     * starts; passing it abandons the attempt rather than leaving the machine
     * parked in ::MqttPicoAssociating.
     */
    absolute_time_t connectionAttemptTimeout;
    int oldStatus; /**< Last link status logged, so an unchanged one stays quiet. */
    /**
     * When the next attempt may start. Set every time the machine returns to
     * ::MqttPicoDisconnected; already reached at boot, so the first attempt is
     * immediate.
     */
    absolute_time_t retryAfter;
    absolute_time_t connectedAt; /**< When the transport last connected. */
};

/* Forward declarations; each is documented at its definition below. */
static void handleError(void *arg, err_t err);
static void updateState(MqttPicoTransport *state, MqttPicoState newState);
static err_t handleReceiveData(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static err_t handleSendData(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t clientConnected(void *arg, struct tcp_pcb *tpcb, err_t err);
static const char *errToString(err_t toConvert);

/**
 * @brief Names a CYW43 link status for logging.
 * @param status Status code from `cyw43_tcpip_link_status()`.
 * @return A static string; `"UNKNOWN"` for an unrecognised code.
 */
/* Forward declaration; documented at its definition below. */
static void dropConnection(MqttPicoTransport *state);

static const char *status_name(int status) {
    switch (status) {
        case CYW43_LINK_DOWN:
            return "LINK_DOWN";
        case CYW43_LINK_JOIN:
            return "LINK_JOIN";
        case CYW43_LINK_NOIP:
            return "LINK_NOIP";
        case CYW43_LINK_UP:
            return "LINK_UP";
        case CYW43_LINK_FAIL:
            return "LINK_FAIL";
        case CYW43_LINK_NONET:
            return "LINK_NONET";
        case CYW43_LINK_BADAUTH:
            return "LINK_BADAUTH";
        default:
            return "UNKNOWN";
    }
}

static MqttPicoTransport *allocateTransport(void) {
    MqttPicoTransport *returnValue = calloc(1, sizeof(MqttPicoTransport));
    if (returnValue == NULL) {
        mqttmsgErrorPrint("failed to allocate connection\n");
        return NULL;
    }

    returnValue->state = MqttPicoDisconnected;
    backoffInit(&returnValue->backoff, RECONNECT_BACKOFF_MIN_MS, RECONNECT_BACKOFF_MAX_MS,
                SESSION_HEALTHY_MS);
    returnValue->retryAfter = get_absolute_time();
    returnValue->oldStatus = CYW43_LINK_DOWN;
    return returnValue;
}

/**
 * @brief lwIP idle-poll callback.
 * @param arg  The connection.
 * @param tpcb The protocol control block being polled.
 * @return `ERR_OK`. Nothing needs doing on an idle connection; the callback
 *         exists so lwIP keeps the poll timer that detects a dead peer.
 */
static err_t tcp_client_poll(void *arg, struct tcp_pcb *tpcb) {
    /* Both parameters are lwIP's to pass, not ours to use: the callback is
       registered purely to keep the poll timer running. */
    (void)arg;
    (void)tpcb;
    return ERR_OK;
}

MqttPicoState mqttPicoConnectionState(MqttPicoTransport *transport) { return transport->state; }

static struct tcp_pcb *getPcb(MqttPicoTransport *state) { return state->tcp_pcb; }

/**
 * @brief Creates the PCB, wires its callbacks and starts the TCP handshake.
 * @param state Connection to open. Moves to ::MqttPicoConnecting on the way.
 * @return true if the handshake started; false if the PCB could not be
 *         created or `tcp_connect()` refused.
 */
static bool openConnection(MqttPicoTransport *state) {
    mqttmsgDebugPrint("Connecting to %s port %u\n", ip4addr_ntoa(&state->server_address),
                      mqttBrokerPort(state->client));
    state->tcp_pcb = tcp_new_ip_type(IP_GET_TYPE(&state->server_address));
    if (!state->tcp_pcb) {
        mqttmsgErrorPrint("failed to create pcb\n");
        return false;
    }

    updateState(state, MqttPicoConnecting);

    tcp_arg(state->tcp_pcb, state);
    tcp_poll(state->tcp_pcb, tcp_client_poll, POLL_TIME_S * 2);
    tcp_err(state->tcp_pcb, handleError);
    tcp_recv(state->tcp_pcb, handleReceiveData);
    tcp_sent(state->tcp_pcb, handleSendData);

    // cyw43_arch_lwip_begin/end should be used around calls into lwIP to ensure correct locking.
    // You can omit them if you are in a callback from lwIP. Note that when using pico_cyw_arch_poll
    // these calls are a no-op and can be omitted, but it is a good practice to use them in
    // case you switch the cyw43_arch type later.
    cyw43_arch_lwip_begin();
    err_t err = tcp_connect(state->tcp_pcb, &state->server_address, mqttBrokerPort(state->client),
                            clientConnected);
    cyw43_arch_lwip_end();

    return err == ERR_OK;
}

/**
 * @brief Moves to a new state and notifies the handler.
 * @param state    Connection to transition.
 * @param newState State to move to.
 */
static void updateState(MqttPicoTransport *state, MqttPicoState newState) {
    state->state = newState;
}

/**
 * @brief DNS resolution callback.
 * @param hostname Name that was looked up.
 * @param ipaddr   Resolved address, or NULL if the lookup failed.
 * @param arg      The connection.
 * @note Moves to ::MqttPicoResolved or ::MqttPicoResolveFailed accordingly.
 */
static void handleDnsResult(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
    /* Only one lookup is ever outstanding, so the name it was for adds nothing
       the connection does not already know. */
    (void)hostname;

    MqttPicoTransport *state = (MqttPicoTransport *)arg;
    if (ipaddr) {
        mqttmsgDebugPrint("server address %s\n", ip4addr_ntoa(ipaddr));
        state->server_address = *ipaddr;
        updateState(state, MqttPicoResolved);
    } else {
        mqttmsgErrorPrint("mqtt dns request failed\n");
        updateState(state, MqttPicoResolveFailed);
    }
}

/**
 * @brief lwIP receive callback: forwards bytes on, or handles a peer close.
 *
 * A NULL @p p is the peer's FIN. The close is completed here and the
 * machine dropped back to ::MqttPicoDisconnected so advanceConnection() reconnects;
 * otherwise it stays wedged at ::MqttPicoConnected forever.
 *
 * @param arg  The connection.
 * @param tpcb The protocol control block.
 * @param p    Received pbuf chain, or NULL if the peer closed.
 * @param err  lwIP receive status.
 * @return Whatever the registered ::ReceiveCallback returns, or `ERR_OK` if
 *         none is registered.
 */
static err_t handleReceiveData(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    MqttPicoTransport *state = (MqttPicoTransport *)arg;
    (void)tpcb;

    if (!p) {
        /* The peer closed. */
        dropConnection(state);
        return ERR_OK;
    }

    /* This is called from lwIP, so cyw43_arch_lwip_begin is not required;
       the check asserts in debug builds if that ever stops being true. */
    cyw43_arch_lwip_check();

    if (p->tot_len > 0) {
        mqttmsgTracePrint("recv %d err %d\n", p->tot_len, err);

        if (p->tot_len > mqttReadRoom(state->client)) {
            /* A frame larger than the buffer can never complete, so waiting
               for the rest of it would block forever. */
            mqttmsgErrorPrint("Receive buffer overflow, closing connection.\n");
            mqttResetStream(state->client);
            pbuf_free(p);
            dropConnection(state);
            return ERR_BUF;
        }

        /* Copied straight out of the pbuf into the client's buffer rather
           than staged here first, to avoid moving up to 2 KB twice. */
        int copied = pbuf_copy_partial(p, mqttReadAt(state->client), p->tot_len, 0);
        bool usable = mqttCommitRead(state->client, copied);
        tcp_recved(tpcb, p->tot_len);

        if (!usable) {
            /* Remaining-length that never terminated: the stream is desynced
               rather than merely short, and no further reading resyncs it. */
            mqttmsgErrorPrint("Malformed MQTT fixed header, closing connection.\n");
            pbuf_free(p);
            dropConnection(state);
            return ERR_VAL;
        }
    }

    pbuf_free(p);
    return ERR_OK;
}

/**
 * @brief lwIP send-acknowledgement callback: forwards to the registered
 *        ::SendCallback.
 * @param arg  The connection.
 * @param tpcb The protocol control block.
 * @param len  How many bytes the peer acknowledged.
 * @return Whatever the callback returns, or `ERR_OK` if none is registered.
 */
static err_t handleSendData(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)tpcb;

    MqttPicoTransport *state = (MqttPicoTransport *)arg;

    (void)state;
    mqttmsgTracePrint("Sending %d bytes.\n", len);

    return ERR_OK;
}

static void advanceConnection(MqttPicoTransport *state) {
    if (state->state == MqttPicoDisconnected) {
        /* Paced rather than retried on every tick. dropConnection() sets the
           deadline; until it passes there is nothing useful to do. */
        if (absolute_time_diff_us(get_absolute_time(), state->retryAfter) > 0) {
            return;
        }

        mqttmsgDebugPrint("Link down, attempting to connect to WiFi\n");
        updateState(state, MqttPicoAssociating);
        state->connectionAttemptTimeout = make_timeout_time_ms(CONNECTION_TIMEOUT_MS);
        // We need to try and make a connection
        if (cyw43_arch_wifi_connect_async(state->wifiSSID, state->wifiPass,
                                          CYW43_AUTH_WPA2_AES_PSK)) {
            mqttmsgErrorPrint("failed to start connecting connect.\n");
        } else {
            mqttmsgDebugPrint("Starting connection process for network %s ..\n", state->wifiSSID);
        }
    } else if (state->state == MqttPicoAssociating &&
               cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP) {
        mqttmsgDebugPrint("Link up.\n");
        updateState(state, MqttPicoLinkUp);
    } else if (state->state == MqttPicoLinkUp) {
        if (ipaddr_aton(mqttBrokerHost(state->client), &state->server_address)) {
            // We parsed an IP address, no need for DNS.
            updateState(state, MqttPicoResolved);
        } else {
            cyw43_arch_lwip_begin();
            int err = dns_gethostbyname(mqttBrokerHost(state->client), &state->server_address,
                                        handleDnsResult, state);
            if (err == ERR_OK) {
                /* Already in lwIP's cache, so server_address is filled in and
                   handleDnsResult() will not be called — the callback fires only
                   on ERR_INPROGRESS. Advancing here is what keeps a cached
                   lookup moving: doing nothing left the state machine in
                   MqttPicoLinkUp, calling this again every iteration and getting the
                   same answer, until the cache entry expired. */
                mqttmsgDebugPrint("dns resolved from cache\n");
                updateState(state, MqttPicoResolved);
            } else if (err != ERR_INPROGRESS) {
                mqttmsgErrorPrint("dns request failed\n");
            } else {
                mqttmsgTracePrint("dns in progress\n");
                updateState(state, MqttPicoResolving);
            }
            cyw43_arch_lwip_end();
        }
    } else if (state->state == MqttPicoResolved) {
        mqttmsgDebugPrint("Link up, starting connection.\n");
        if (!openConnection(state)) {
            /* A synchronous tcp_connect failure gets no tcp_err callback, so
               without this the machine sits in MqttPicoConnecting forever. */
            mqttmsgErrorPrint("Failed to initialize connection to server.\n");
            dropConnection(state);
        }
    } else if (state->state == MqttPicoResolveFailed) {
        /* This state must reach a branch: left unhandled with the link up, the
           machine parks here, and a single failed lookup keeps the device off
           MQTT until it is power-cycled. */
        mqttmsgErrorPrint("DNS lookup failed, will retry.\n");
        dropConnection(state);
    } else if (state->state != MqttPicoDisconnected && state->state != MqttPicoAssociating &&
               cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) {
        mqttmsgDebugPrint("Link down, initiating connection closure.\n");
        dropConnection((MqttPicoTransport *)state);
    } else if (state->state == MqttPicoConnected) {
    } else if (state->state == MqttPicoAssociating) {
        int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (status != state->oldStatus) {
            // Only log when the status has changed.
            mqttmsgTracePrint("%s\n", status_name(status));
            state->oldStatus = status;
        }

        if (absolute_time_diff_us(state->connectionAttemptTimeout, get_absolute_time()) > 0) {
            mqttmsgErrorPrint(
                "Connection has not succeeded within timeout period, resetting connection.\n");
            dropConnection((MqttPicoTransport *)state);
        }
    }
}

/**
 * @brief TCP connect callback: records the connect time and moves to
 *        ::MqttPicoConnected.
 * @param arg  The connection.
 * @param tpcb The protocol control block.
 * @param err  Handshake result.
 * @return `ERR_OK`, or @p err if the handshake failed.
 */
static err_t clientConnected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)tpcb;

    MqttPicoTransport *state = (MqttPicoTransport *)arg;
    if (err != ERR_OK) {
        mqttmsgErrorPrint("connect failed %d\n", err);
        return err;
    }

    mqttmsgDebugPrint("TCP connection to server.\n");

    state->connectedAt = get_absolute_time();
    updateState(state, MqttPicoConnected);

    return ERR_OK;
}

/**
 * @brief lwIP fatal-error callback: tears the connection down.
 *
 * The PCB is already gone by the time lwIP calls this, so dropConnection()
 * is what arms the backoff and returns the machine to ::MqttPicoDisconnected.
 *
 * @param arg The connection.
 * @param err The error that killed it.
 */
static void handleError(void *arg, err_t err) {
    mqttmsgTracePrint("tcp_client_err %s\n", errToString(err));

    // Reset connection.
    mqttmsgDebugPrint("Closing connection\n");
    dropConnection((MqttPicoTransport *)arg);
}

static void dropConnection(MqttPicoTransport *state) {
    mqttmsgDebugPrint("Client closed, cleaning up.\n");

    if (state->tcp_pcb != NULL) {
        tcp_arg(state->tcp_pcb, NULL);
        tcp_poll(state->tcp_pcb, NULL, 0);
        tcp_sent(state->tcp_pcb, NULL);
        tcp_recv(state->tcp_pcb, NULL);
        tcp_err(state->tcp_pcb, NULL);
        err_t err = tcp_close(state->tcp_pcb);
        if (err != ERR_OK) {
            mqttmsgErrorPrint("close failed %d, calling abort\n", err);
            tcp_abort(state->tcp_pcb);
            err = ERR_ABRT;
        }
        state->tcp_pcb = NULL;
    }

    /* One call per finished attempt, whatever ended it — a refused connection,
       a failed lookup, a dropped session. Only a session that reached the
       broker and lasted earns the wait back down. */
    unsigned sessionMs = 0;
    if (mqttSessionIsEstablished(state->client)) {
        int64_t heldUs = absolute_time_diff_us(state->connectedAt, get_absolute_time());
        sessionMs = heldUs > 0 ? (unsigned)(heldUs / 1000) : 0;
    }

    unsigned waitMs =
        backoffOnAttemptEnded(&state->backoff, mqttSessionIsEstablished(state->client), sessionMs);
    state->retryAfter = make_timeout_time_ms(waitMs);
    mqttmsgDebugPrint("Reconnecting in %ums.\n", waitMs);

    updateState(state, MqttPicoDisconnected);
}

/**
 * @brief Names an lwIP error code for logging.
 * @param toConvert Error code.
 * @return A static string; `"UNKNOWN"` for an unrecognised code.
 */
static const char *errToString(err_t toConvert) {
    switch (toConvert) {
        case ERR_OK: {
            return "OK";
        }

        case ERR_MEM: {
            return "MEM";
        }

        case ERR_BUF: {
            return "BUF";
        }

        case ERR_TIMEOUT: {
            return "TIMEOUT";
        }

        case ERR_RTE: {
            return "RTE";
        }

        case ERR_INPROGRESS: {
            return "INPROGRESS";
        }

        case ERR_VAL: {
            return "VAL";
        }

        case ERR_WOULDBLOCK: {
            return "WOULDBLOCK";
        }

        case ERR_USE: {
            return "USE";
        }

        case ERR_ALREADY: {
            return "ALREADY";
        }

        case ERR_ISCONN: {
            return "ISCONN";
        }

        case ERR_CONN: {
            return "CONN";
        }

        case ERR_IF: {
            return "IF";
        }

        case ERR_ABRT: {
            return "ABRT";
        }

        case ERR_RST: {
            return "RST";
        }

        case ERR_CLSD: {
            return "CLSD";
        }

        case ERR_ARG: {
            return "ARG";
        }

        default: {
            return "UNKNOWN";
        }
    }
}

void mqttPicoSetWifi(MqttPicoTransport *transport, const char *ssid, const char *password) {
    transport->wifiSSID = ssid;
    transport->wifiPass = password;
}

/* ── The MqttTransport interface ───────────────────────────────────── */

/**
 * @brief Assembles a 32-bit random word from the ring oscillator.
 * @return One random word, built one bit per read of the ROSC random bit.
 */
static uint32_t roscWord(void) {
    int k, random = 0;
    volatile uint32_t *rnd_reg = (uint32_t *)(ROSC_BASE + ROSC_RANDOMBIT_OFFSET);

    for (k = 0; k < 32; k++) {
        random = random << 1;
        random = random + (0x00000001 & (*rnd_reg));
    }
    return (uint32_t)random;
}

/**
 * @brief Random bytes from the ring oscillator.
 * @param ctx    Unused; the oscillator is the board's.
 * @param out    Where to write @p length bytes.
 * @param length How many bytes are wanted.
 */
static void picoRandomFill(void *ctx, uint8_t *out, size_t length) {
    (void)ctx;
    for (size_t i = 0; i < length; i++) {
        out[i] = (uint8_t)roscWord();
    }
}

/**
 * @brief Queues bytes on the TCP connection.
 *
 * `ERR_MEM` is the one failure worth distinguishing: lwIP's send queue is
 * momentarily full, the connection is fine, and the next write has every
 * chance of succeeding. Anything else means the connection is not carrying
 * bytes any more.
 *
 * @param ctx    The transport, as a `void*`.
 * @param bytes  Bytes to queue.
 * @param length How many bytes to queue.
 * @return How the write went.
 */
static MqttWriteStatus picoWrite(void *ctx, const uint8_t *bytes, uint32_t length) {
    MqttPicoTransport *state = (MqttPicoTransport *)ctx;

    if (getPcb(state) == NULL) {
        return MqttWriteFailed;
    }

    cyw43_arch_lwip_begin();
    err_t err = tcp_write(getPcb(state), bytes, (u16_t)length, TCP_WRITE_FLAG_COPY);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) {
        return MqttWriteOk;
    }

    if (err == ERR_MEM) {
        return MqttWriteBusy;
    }

    mqttmsgErrorPrint("tcp_write failed (%d).\n", err);
    return MqttWriteFailed;
}

/**
 * @brief Ends the current connection at the client's request.
 * @param ctx The transport, as a `void*`.
 */
static void picoClose(void *ctx) { dropConnection((MqttPicoTransport *)ctx); }

/**
 * @brief The transport's clock.
 * @param ctx Unused; the clock is the board's.
 * @return Milliseconds since boot.
 */
static uint32_t picoNowMs(void *ctx) {
    (void)ctx;
    return to_ms_since_boot(get_absolute_time());
}

/**
 * @brief Whether bytes can currently flow.
 * @param ctx The transport, as a `void*`.
 * @return true once TCP is up.
 */
static bool picoIsConnected(void *ctx) {
    return ((MqttPicoTransport *)ctx)->state == MqttPicoConnected;
}

/**
 * @brief Advances the connection by one step.
 *
 * Received bytes do not arrive here — lwIP delivers them from its own
 * callback, which is why this only has to service the state machine.
 *
 * @param ctx    The transport, as a `void*`.
 * @param client The client being driven.
 */
static void picoPoll(void *ctx, MqttClient *client) {
    MqttPicoTransport *state = (MqttPicoTransport *)ctx;
    (void)client;

    MqttPicoState before = state->state;
    advanceConnection(state);

    /* The CONNECT goes out when TCP has just come up, and only then: lwIP
       reports the transition once, and repeating it would open a second
       session over the first. */
    if (state->state == MqttPicoConnected && before != MqttPicoConnected) {
        mqttConnectionUp(state->client);
    }
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

MqttPicoTransport *mqttPicoTransportNew(void) { return allocateTransport(); }

void mqttPicoTransportFree(MqttPicoTransport *transport) {
    if (transport == NULL) {
        return;
    }

    dropConnection(transport);
    free(transport);
}

void mqttPicoTransportAttach(MqttPicoTransport *transport, MqttClient *client) {
    transport->client = client;

    MqttTransport iface = {
        .write = picoWrite,
        .close = picoClose,
        .nowMs = picoNowMs,
        .poll = picoPoll,
        .isConnected = picoIsConnected,
        .randomFill = picoRandomFill,
        .ctx = transport,
    };
    mqttAttach(client, iface);
}
