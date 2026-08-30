/**
 * @file client.c
 * @brief The client: configuration, subscriptions, the receive stream, and
 *        the step that drives all of it.
 *
 * Everything that is neither the frame codec below it nor the transport
 * beneath that. Both platforms run this same file; what they supply is an
 * ::MqttTransport.
 *
 * @see mqttmsg.h
 */

#include <mqttmsg/mqttmsg.h>

#include <stdlib.h>
#include <string.h>

#include <mqttmsg/logger.h>
#include <mqttmsg/mqtt.h>

#include "framing.h"
#include "router.h"
#include "session.h"

/** @brief Size of the per-client receive buffer, in bytes. */
#define BUF_SIZE 2048

/** @brief How many entries the subscription table grows by at a time. */
#define SUBSCRIPTION_GROWTH 5

/** @brief How long a poll waits for bytes unless told otherwise, in ms. */
#define DEFAULT_POLL_BUDGET_MS 100

/** @brief One registered filter and the callback it routes to. */
typedef struct {
    const char* filter;      /**< Topic filter, as registered. */
    MqttMessageFn onMessage; /**< Called for each matching PUBLISH. */
    void* userData;          /**< Handed back to @c onMessage. */
} MqttRoute;

/** @brief The structure behind MqttClient. */
struct MqttClient {
    MqttSession session;      /**< What to send, and when. */
    MessageFramer framer;     /**< Reassembles the read stream. */
    uint8_t buffer[BUF_SIZE]; /**< Storage the framer works in. */
    MqttTransport transport;  /**< How bytes leave and arrive. */
    bool hasTransport;        /**< Whether one has been attached. */

    MqttRoute* routes; /**< Subscription table. */
    int routeCount;    /**< Entries in use. */
    int routeSize;     /**< Entries allocated. */

    /**
     * Per-type callbacks, indexed by ::MessageType. Sixteen because the
     * type is four bits, so no frame can index past the end.
     */
    MqttEventFn onEvent[16];
    MqttEventFn onIdle; /**< Called when no frame is waiting. */

    const char* brokerHost; /**< Broker hostname. */
    uint16_t brokerPort;    /**< Broker TCP port. */
    const char* username;   /**< CONNECT username, or NULL. */
    const char* password;   /**< CONNECT password, or NULL. */
    const char* clientId;   /**< Caller's client id, or NULL. */
    /**
     * Fallback identifier, generated once when the client is created and
     * reused for every CONNECT after. 23 characters plus a terminator,
     * which is the longest MQTT 3.1 requires a broker to accept.
     */
    char generatedClientId[24];
    const char* willTopic;   /**< Last-will topic, or NULL. */
    const char* willMessage; /**< Last-will payload. */
    bool willRetain;         /**< Broker retains the will. */

    int pollBudgetMs; /**< How long a transport's poll may wait. */
    void* data;       /**< Caller's context pointer. */
};

/* Forward declaration; documented at its definition below. */
static void onFramed(void* ctx, FixedHeader* fh, MqttPayload* frame);

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/**
 * @brief Fills @p out from the transport's random source.
 *
 * The transport is asked because the platforms disagree about where random
 * bytes come from — a ring oscillator on the device, the kernel on a host —
 * and the client has no business knowing which it is sitting on.
 *
 * A transport that offers none falls back to the clock. That is weak, and
 * it is only ever used to name a client to a broker, but it is worth
 * knowing that a platform which also has a coarse clock could hand two
 * devices the same identifier.
 *
 * @param client Client whose transport supplies the bytes.
 * @param out    Where to write @p length bytes.
 * @param length How many bytes are wanted.
 */
static void transportRandomFill(void* client, uint8_t* out, size_t length) {
    MqttClient* c = (MqttClient*)client;

    if (c->transport.randomFill != NULL) {
        c->transport.randomFill(c->transport.ctx, out, length);
        return;
    }

    uint32_t seed = c->transport.nowMs(c->transport.ctx) ^ 0x9E3779B9u;
    for (size_t i = 0; i < length; i++) {
        seed = seed * 1664525u + 1013904223u;
        out[i] = (uint8_t)(seed >> 24);
    }
}

MqttClient* mqttNew(void) {
    MqttClient* client = calloc(1, sizeof(MqttClient));
    if (client == NULL) {
        mqttmsgErrorPrint("Out of memory creating a client.\n");
        return NULL;
    }

    client->framer.buffer = client->buffer;
    client->framer.capacity = BUF_SIZE;
    client->framer.length = 0;
    client->framer.onMessage = onFramed;
    client->framer.ctx = client;

    client->brokerPort = 1883;
    client->pollBudgetMs = DEFAULT_POLL_BUDGET_MS;

    return client;
}

void mqttFree(MqttClient* client) {
    if (client == NULL) {
        return;
    }

    free(client->routes);
    free(client);
}

void mqttAttach(MqttClient* client, MqttTransport transport) {
    client->transport = transport;
    client->hasTransport = true;

    /* The session takes the transport as it stands; it only ever calls
       write, close and nowMs. Keep-alive stays off until
       mqttSetKeepAlive() names a period. */
    mqttSessionInit(&client->session, transport, 0);
}

/* ── Configuration ─────────────────────────────────────────────────── */

void mqttSetBroker(MqttClient* client, const char* host, uint16_t port) {
    client->brokerHost = host;
    client->brokerPort = port;
}

void mqttSetCredentials(MqttClient* client, const char* username, const char* password) {
    client->username = username;
    client->password = password;
}

void mqttSetClientId(MqttClient* client, const char* clientId) { client->clientId = clientId; }

void mqttSetWill(MqttClient* client, const char* topic, const char* message, bool retain) {
    client->willTopic = topic;
    client->willMessage = message;
    client->willRetain = retain;
}

void mqttSetKeepAlive(MqttClient* client, uint16_t seconds) {
    mqttSessionSetKeepAlive(&client->session, seconds);
}

void mqttSetPollBudget(MqttClient* client, int milliseconds) {
    client->pollBudgetMs = milliseconds < 0 ? 0 : milliseconds;
}

void mqttSetData(MqttClient* client, void* data) { client->data = data; }

void* mqttGetData(MqttClient* client) { return client->data; }

void mqttOnEvent(MqttClient* client, MessageType type, MqttEventFn onEvent) {
    if ((unsigned)type < 16u) {
        client->onEvent[type] = onEvent;
    }
}

void mqttOnIdle(MqttClient* client, MqttEventFn onIdle) { client->onIdle = onIdle; }

const char* mqttClientId(MqttClient* client) {
    return client->clientId != NULL ? client->clientId : client->generatedClientId;
}

const char* mqttBrokerHost(MqttClient* client) { return client->brokerHost; }
uint16_t mqttBrokerPort(MqttClient* client) { return client->brokerPort; }
int mqttPollBudget(MqttClient* client) { return client->pollBudgetMs; }
bool mqttSessionIsEstablished(MqttClient* client) { return client->session.sessionEstablished; }

bool mqttIsConnected(MqttClient* client) {
    return client->hasTransport && client->transport.isConnected(client->transport.ctx);
}

/* ── Subscriptions ─────────────────────────────────────────────────── */

bool mqttSubscribe(MqttClient* client, const char* filter, MqttMessageFn onMessage,
                   void* userData) {
    if (client->routeCount == client->routeSize) {
        MqttRoute* grown =
            calloc((size_t)client->routeSize + SUBSCRIPTION_GROWTH, sizeof(MqttRoute));
        if (grown == NULL) {
            /* Nothing is published until the table is known good: on failure
               the subscriptions already registered stay registered. */
            mqttmsgErrorPrint("Out of memory growing the subscription table.\n");
            return false;
        }

        memcpy(grown, client->routes, sizeof(MqttRoute) * (size_t)client->routeCount);
        free(client->routes);
        client->routes = grown;
        client->routeSize += SUBSCRIPTION_GROWTH;
    }

    MqttRoute* slot = &client->routes[client->routeCount];
    slot->filter = filter;
    slot->onMessage = onMessage;
    slot->userData = userData;
    client->routeCount++;
    return true;
}

/**
 * @brief Supplies a registered filter to the SUBSCRIBE builder.
 * @param ctx   The client, as a `void*`.
 * @param index Which registered subscription is wanted.
 * @return Its topic filter.
 */
static const char* subscribeFilterAt(void* ctx, int index) {
    return ((MqttClient*)ctx)->routes[index].filter;
}

/**
 * @brief Sends one SUBSCRIBE covering everything registered.
 * @param client Client to subscribe on.
 */
static void sendSubscriptions(MqttClient* client) {
    if (client->routeCount <= 0) {
        return;
    }

    if (mqttSessionSendSubscribe(&client->session, subscribeFilterAt, client, client->routeCount) ==
        MqttWriteOk) {
        mqttmsgDebugPrint("Sent subscribe message.\n");
    }
}

/* ── Dispatch ──────────────────────────────────────────────────────── */

/**
 * @brief What routing one PUBLISH needs to reach a callback.
 *
 * The router hands a visitor an index and a topic; the body it came with
 * rides here, because the router has no reason to know what a payload is.
 */
typedef struct {
    MqttClient* client; /**< Client holding the subscription table. */
    MqttPayload body;   /**< Message body, as parsed. */
} PublishRouting;

static const char* routeFilterAt(void* ctx, int index) {
    return ((PublishRouting*)ctx)->client->routes[index].filter;
}

static void routeToSubscriber(void* ctx, int index, MqttString topic) {
    PublishRouting* routing = (PublishRouting*)ctx;
    MqttRoute* route = &routing->client->routes[index];

    if (route->onMessage != NULL) {
        route->onMessage(routing->client, topic, routing->body, route->userData);
    }
}

/**
 * @brief Routes a PUBLISH to every subscription whose filter covers it.
 * @param client Client the message arrived on.
 * @param fh     Fixed header of the message.
 * @param frame  The whole frame.
 */
static void routePublish(MqttClient* client, FixedHeader* fh, MqttPayload* frame) {
    if (client->routeCount <= 0) {
        return;
    }

    Message m = parseMessage(*fh, *frame);
    if (m.status != MessageOk) {
        /* Dispatching would hand the callback views over bytes that either
           never arrived or contradict the frame's own length. */
        mqttmsgErrorPrint("Discarding an unusable publish.\n");
        return;
    }

    PublishRouting routing = {.client = client, .body = m.payload};
    mqttRouteDispatch(m.variableHeader.topicName, routeFilterAt, routeToSubscriber, &routing,
                      client->routeCount);
}

/**
 * @brief Routes one completed frame.
 * @param ctx   The client, as a `void*`.
 * @param fh    Fixed header of the completed message.
 * @param frame The completed frame.
 */
static void onFramed(void* ctx, FixedHeader* fh, MqttPayload* frame) {
    MqttClient* client = (MqttClient*)ctx;

    /* The CONNACK is answered before the caller sees it. Reconnect pacing
       keys its reset off the broker accepting us rather than off the
       transport connecting, and the answer decides whether there is a
       session to subscribe on — neither of which the caller's callback
       should be able to change by taking its time. */
    bool accepted = false;
    if (fh->type == ConnAck) {
        mqttmsgDebugPrint("Connected to MQTT server.\n");
        accepted = mqttSessionAcceptConnAck(&client->session, *frame);
    }

    if ((unsigned)fh->type < 16u && client->onEvent[fh->type] != NULL) {
        client->onEvent[fh->type](client, *frame);
    }

    if (fh->type == ConnAck) {
        /* Only on a session the broker agreed to. A SUBSCRIBE down a refused
           connection is a frame sent into a socket the broker is closing. */
        if (accepted) {
            sendSubscriptions(client);
        }
    } else if (fh->type == Publish) {
        MQTTMSG_DUMP_PAYLOAD(*frame);
        routePublish(client, fh, frame);
    }
}

/* ── The receive stream, for a transport ───────────────────────────── */

int mqttReadRoom(MqttClient* client) { return framerRemaining(&client->framer); }

uint8_t* mqttReadAt(MqttClient* client) { return client->framer.buffer + client->framer.length; }

bool mqttCommitRead(MqttClient* client, int count) {
    /* Bytes at all from the broker prove the connection is still carrying
       them, whatever they turn out to say. */
    mqttSessionNoteTraffic(&client->session);
    return framerConsume(&client->framer, count) != FramingMalformed;
}

void mqttResetStream(MqttClient* client) { client->framer.length = 0; }

/* ── Running ───────────────────────────────────────────────────────── */

void mqttConnectionUp(MqttClient* client) {
    /* A fresh connection carries no relationship to whatever was buffered on
       the last one; framing from a stale partial message corrupts the
       stream. */
    mqttResetStream(client);
    mqttSessionConnectionUp(&client->session);

    /* Once per client rather than once per CONNECT, so a device that
       reconnects is recognisably the same one. Deferred to here rather than
       done in mqttNew(), because the randomness comes from the transport and
       there is none attached yet at that point. */
    if (client->clientId == NULL && client->generatedClientId[0] == '\0') {
        mqttBuildClientId(client->generatedClientId, sizeof(client->generatedClientId),
                          transportRandomFill, client);
    }

    const char* clientId = mqttClientId(client);
    mqttmsgDebugPrint("ConnectionId %s\n", clientId);

    mqttSessionSendConnect(&client->session, clientId, client->username, client->password,
                           client->willTopic, client->willMessage, client->willRetain);
}

void mqttPublish(MqttClient* client, const char* topic, const char* message, bool retain) {
    mqttSessionPublishString(&client->session, topic, message, retain);
}

void mqttPoll(MqttClient* client) {
    if (!client->hasTransport) {
        return;
    }

    client->transport.poll(client->transport.ctx, client);

    if (client->transport.isConnected(client->transport.ctx)) {
        mqttSessionServiceKeepAlive(&client->session);
    }

    if (client->onIdle != NULL) {
        client->onIdle(client, mqttPayloadNone());
    }
}

void mqttPollRLM(void* client) { mqttPoll((MqttClient*)client); }

/**
 * @brief Whether the transport has given up.
 * @param client Client to ask about.
 * @return Its transport's answer, or false if it does not implement the
 *         query — a transport that only backs off and retries never
 *         reaches a state worth asking about.
 */
static bool transportHasStopped(MqttClient* client) {
    if (client->transport.isStopped == NULL) {
        return false;
    }

    return client->transport.isStopped(client->transport.ctx);
}

void mqttRun(MqttClient* client) {
    /* A stopped transport's poll returns without doing anything and without
       ever disconnecting, so looping on hasTransport alone burned a core
       calling it — rather than returning, which is what this promises. */
    while (client->hasTransport && !transportHasStopped(client)) {
        mqttPoll(client);
    }
}
