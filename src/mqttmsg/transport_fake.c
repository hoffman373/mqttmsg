/**
 * @file transport_fake.c
 * @brief A transport made of arrays and a hand-wound clock.
 *
 * @see transport_fake.h
 */

#include <mqttmsg/transport_fake.h>

#include <stdlib.h>
#include <string.h>

#include <mqttmsg/mqtt.h>

/** @brief How many written frames are remembered. */
#define MAX_WRITES 32
/** @brief Bytes remembered per written frame. */
#define MAX_FRAME 256
/** @brief Bytes queued for delivery but not yet read. */
#define INBOX_SIZE 2048

/** @brief The structure behind MqttFakeTransport. */
struct MqttFakeTransport {
    MqttClient* client; /**< The client being driven. */

    uint8_t written[MAX_WRITES][MAX_FRAME]; /**< Frames the client wrote. */
    uint32_t writtenLength[MAX_WRITES];     /**< How long each of them was. */
    int writeCount;                         /**< How many it wrote. */
    MqttWriteStatus writeStatus;            /**< What write() reports. */

    uint8_t inbox[INBOX_SIZE]; /**< Bytes queued for the client to read. */
    int inboxLength;           /**< How many are waiting. */

    int closeCount;   /**< How often the client asked to disconnect. */
    uint32_t clockMs; /**< The clock, moved only by the caller. */
    bool connected;   /**< Whether bytes can flow. */
    bool stopped;     /**< Whether the transport has given up, as mqttFakeStop() says. */
};

/* ── The MqttTransport interface ───────────────────────────────────── */

static MqttWriteStatus fakeWrite(void* ctx, const uint8_t* bytes, uint32_t length) {
    MqttFakeTransport* t = (MqttFakeTransport*)ctx;

    if (t->writeStatus == MqttWriteOk && t->writeCount < MAX_WRITES && length <= MAX_FRAME) {
        memcpy(t->written[t->writeCount], bytes, length);
        t->writtenLength[t->writeCount] = length;
        t->writeCount++;
    }

    return t->writeStatus;
}

static void fakeClose(void* ctx) {
    MqttFakeTransport* t = (MqttFakeTransport*)ctx;
    t->closeCount++;
    t->connected = false;
}

static uint32_t fakeNowMs(void* ctx) { return ((MqttFakeTransport*)ctx)->clockMs; }

static bool fakeIsConnected(void* ctx) { return ((MqttFakeTransport*)ctx)->connected; }

/**
 * @brief A fixed ramp, so a test that looks at the generated client id sees
 *        the same one every run.
 * @param ctx    Unused.
 * @param out    Where to write @p length bytes.
 * @param length How many bytes are wanted.
 */
static void fakeRandomFill(void* ctx, uint8_t* out, size_t length) {
    (void)ctx;
    for (size_t i = 0; i < length; i++) {
        out[i] = (uint8_t)i;
    }
}

/**
 * @brief Hands the client whatever was queued, the way a real read does.
 * @param ctx    The transport, as a `void*`.
 * @param client The client being driven.
 */
static void fakePoll(void* ctx, MqttClient* client) {
    MqttFakeTransport* t = (MqttFakeTransport*)ctx;

    if (!t->connected || t->inboxLength == 0) {
        return;
    }

    int room = mqttReadRoom(client);
    int count = t->inboxLength < room ? t->inboxLength : room;
    if (count <= 0) {
        return;
    }

    memcpy(mqttReadAt(client), t->inbox, (size_t)count);

    /* Whatever was not taken stays queued, so a test can hand over more than
       one read's worth and watch it arrive in pieces. */
    memmove(t->inbox, t->inbox + count, (size_t)(t->inboxLength - count));
    t->inboxLength -= count;

    if (!mqttCommitRead(client, count)) {
        t->connected = false;
    }
}

/**
 * @brief Whether the transport has given up.
 * @param ctx The transport, as a `void*`.
 * @return What mqttFakeStop() last set; false until a test says otherwise.
 */
static bool fakeIsStopped(void* ctx) { return ((MqttFakeTransport*)ctx)->stopped; }

/* ── Lifecycle ─────────────────────────────────────────────────────── */

MqttFakeTransport* mqttFakeTransportNew(void) {
    MqttFakeTransport* t = calloc(1, sizeof(MqttFakeTransport));
    if (t == NULL) {
        return NULL;
    }

    t->writeStatus = MqttWriteOk;
    return t;
}

void mqttFakeTransportFree(MqttFakeTransport* t) { free(t); }

void mqttFakeTransportAttach(MqttFakeTransport* t, MqttClient* client) {
    t->client = client;

    MqttTransport iface = {
        .write = fakeWrite,
        .close = fakeClose,
        .nowMs = fakeNowMs,
        .poll = fakePoll,
        .isConnected = fakeIsConnected,
        .isStopped = fakeIsStopped,
        .randomFill = fakeRandomFill,
        .ctx = t,
    };
    mqttAttach(client, iface);
}

/* ── Driving it from a test ────────────────────────────────────────── */

void mqttFakeConnect(MqttFakeTransport* t) {
    t->connected = true;
    mqttConnectionUp(t->client);
}

void mqttFakeDeliver(MqttFakeTransport* t, MqttPayload frame) {
    uint32_t length = mqttPayloadIsOk(frame) ? mqttPayloadLength(frame) : 0;
    if (length == 0 || t->inboxLength + (int)length > INBOX_SIZE) {
        return;
    }

    memcpy(t->inbox + t->inboxLength, mqttPayloadBytes(frame), length);
    t->inboxLength += (int)length;
}

void mqttFakeAdvanceClock(MqttFakeTransport* t, uint32_t milliseconds) {
    t->clockMs += milliseconds;
}

int mqttFakeWriteCount(MqttFakeTransport* t) { return t->writeCount; }

int mqttFakeWrittenType(MqttFakeTransport* t, int index) {
    if (index < 0 || index >= t->writeCount) {
        return -1;
    }
    return (int)parseFixedHeader(mqttFakeWritten(t, index)).type;
}

MqttPayload mqttFakeWritten(MqttFakeTransport* t, int index) {
    if (index < 0 || index >= t->writeCount) {
        return mqttPayloadNone();
    }
    return mqttPayloadFromBytes(t->written[index], t->writtenLength[index]);
}

int mqttFakeCloseCount(MqttFakeTransport* t) { return t->closeCount; }

void mqttFakeSetWriteStatus(MqttFakeTransport* t, MqttWriteStatus status) {
    t->writeStatus = status;
}

void mqttFakeStop(MqttFakeTransport* t) {
    t->stopped = true;
    t->connected = false;
}
