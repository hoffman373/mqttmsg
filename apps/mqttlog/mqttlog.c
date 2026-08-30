/**
 * @file mqttlog.c
 * @brief A command-line MQTT subscriber that timestamps and prints what it
 *        receives.
 *
 * The reference consumer of the host client, and the thing to reach for
 * when a device's traffic needs watching.
 *
 * @code
 * mqttlog -a broker.local -p 1883 -s 'home/#' -u user -m pass
 * @endcode
 *
 * See prog_args.h for the full option list.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mqttmsg/mqttmsg.h>
#include <mqttmsg/transport_socket.h>

#include "prog_args.h"

/** @brief Set zero to silence #log_print. */
#define LOG 1

/** @brief Scratch buffer for the formatted timestamp #log_print prepends. */
static char str_date[256];

/**
 * @def log_print
 * @brief Prints a line prefixed with the local date and time.
 * @param fmt printf format string, followed by its arguments.
 */
#define log_print(fmt, ...)                                          \
    do {                                                             \
        if (LOG) {                                                   \
            time_t rawtime;                                          \
            struct tm* timeinfo;                                     \
            time(&rawtime);                                          \
            timeinfo = localtime(&rawtime);                          \
            strftime(str_date, sizeof(str_date), "%x %X", timeinfo); \
            printf("%s ", str_date);                                 \
            printf(fmt, ##__VA_ARGS__);                              \
        }                                                            \
    } while (0);

/* Forward declarations; each is documented at its definition below. */
void onSubscribe(MqttClient* client, MqttPayload recvdMsg);
void onMessage(MqttClient* client, MqttString topic, MqttPayload body, void* userData);

/**
 * @brief Parses the arguments, wires the callbacks and runs the client.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 1 if the arguments could not be parsed. Otherwise does not
 *         return: mqttRun() runs until the process is killed.
 */
int main(int argc, char** argv) {
    ProgArgs arguments;
    if (!mqttParseArgs(argc, argv, &arguments)) {
        printf("Error parsing arguments.");
        return 1;
    }

    dumpDisplay(&arguments);

    MqttSocketTransport* wire = mqttSocketTransportNew();
    if (wire == NULL) {
        puts("Could not create the socket transport.");
        return 1;
    }

    MqttClient* mqtt = mqttNew();
    if (mqtt == NULL) {
        puts("Could not create the MQTT client.");
        mqttSocketTransportFree(wire);
        return 1;
    }

    mqttSocketTransportAttach(wire, mqtt);

    mqttSetBroker(mqtt, arguments.mqttServerAddress, (uint16_t)arguments.mqttServerPort);
    mqttSetCredentials(mqtt, arguments.username, arguments.password);
    mqttSetKeepAlive(mqtt, 90);
    if (arguments.clientId != NULL) {
        mqttSetClientId(mqtt, arguments.clientId);
    }

    mqttOnEvent(mqtt, SubAck, onSubscribe);

    /* Registered up front; the client sends one SUBSCRIBE covering all of
       them on every connect, so there is nothing to do on CONNACK. */
    for (int i = 0; i < arguments.numSubscriptions; i++) {
        if (!mqttSubscribe(mqtt, arguments.subscriptions[i], onMessage, NULL)) {
            puts("Could not register a subscription.");
            return 1;
        }
    }

    mqttRun(mqtt);
}

/**
 * @brief Logs that the broker accepted the subscriptions.
 * @param client   The client; unused.
 * @param recvdMsg The SUBACK body; unused.
 */
void onSubscribe(MqttClient* client, MqttPayload recvdMsg) {
    (void)client;
    (void)recvdMsg;
    log_print("Successfully subscribed.\n");
}

/**
 * @brief Prints a received message's topic and body, timestamped.
 * @param client   The client; unused.
 * @param topic    The topic it arrived on, which for a wildcard
 *                 subscription is the real one rather than the filter.
 * @param body     The message body.
 * @param userData Unused; every subscription here registers the same
 *                 callback with nothing attached.
 */
void onMessage(MqttClient* client, MqttString topic, MqttPayload body, void* userData) {
    (void)client;
    (void)userData;
    log_print("Received publish. Topic " MQTT_STR_FMT " Body " MQTT_STR_FMT "\n",
              MQTT_STR_ARG(topic), (int)mqttPayloadLength(body),
              (const char*)mqttPayloadBytes(body));
}
