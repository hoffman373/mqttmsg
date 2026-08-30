/**
 * @file prog_args.c
 * @brief `getopt`-based command-line parsing for the host tools.
 *
 * @see prog_args.h
 */

#include "prog_args.h"
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/**
 * @brief Points @p dest at localhost:1883 with no credentials.
 * @param dest Configuration to fill in.
 * @note The name is misspelled and kept that way; it is file-static.
 */
static void setDeafults(ProgArgs* dest) {
    dest->mqttServerAddress = "127.0.0.1";
    dest->mqttServerPort = 1883;
    dest->username = NULL;
    dest->password = NULL;
}

/** @brief Prints the option summary to stdout. */
static void dumpHelp(void) {
    puts("-h Prints help.");
    puts("-a <value> Address of MQTT server.");
    puts("-p <value> Port used by MQTT server.");
    puts("-s <value> Subscription topic. Multiple possible.");
    puts("-m <value> Password used for authentication with MQTT server.");
    puts("-u <value> Username used for authentication with MQTT server.");
    puts("-i <value> Client identifier. A random one is generated if omitted.");
}

int mqttParseArgs(int argc, char** argv, ProgArgs* dest) {
    memset(dest, 0, sizeof(ProgArgs));
    setDeafults(dest);
    return mqttParseArgsWithDefaults(argc, argv, dest);
}

/**
 * @brief Releases whatever was allocated before a parse gave up.
 * @param dest Configuration to clear, so a caller that gets 0 back is not
 *             left holding half-filled ProgArgs.
 */
static void discardArgs(ProgArgs* dest) {
    free(dest->subscriptions);
    dest->subscriptions = NULL;
    dest->numSubscriptions = 0;

    free(dest->username);
    dest->username = NULL;

    free(dest->password);
    dest->password = NULL;
}

int mqttParseArgsWithDefaults(int argc, char** argv, ProgArgs* dest) {
    int c;
    int numSubscriptions = 0;

    /* Every -s occupies at least one argv slot, so argc bounds how many
       there can be. Sizing from that up front collects them in a single
       getopt pass; counting them first would mean scanning argv twice and
       resetting optind in between. The slack is a few pointers. */
    dest->subscriptions = calloc(argc + 1, sizeof(char*));
    if (!dest->subscriptions) {
        fprintf(stderr, "Failed to allocate memory\n");
        return 0;
    }

    while ((c = getopt(argc, argv, "ha:p:s:m:u:i:")) != -1) {
        switch (c) {
            case 'h':
                dumpHelp();
                discardArgs(dest);
                return 0;
            case 'a':
                dest->mqttServerAddress = optarg;
                break;
            case 'p':
                dest->mqttServerPort = atoi(optarg);
                break;
            case 's':
                dest->subscriptions[numSubscriptions++] = optarg;
                break;
            case 'm':
                dest->password = calloc(strlen(optarg) + 1, sizeof(char));
                if (!dest->password) {
                    fprintf(stderr, "Failed to allocate memory\n");
                    discardArgs(dest);
                    return 0;
                }

                strcpy(dest->password, optarg);
                break;
            case 'u':
                dest->username = calloc(strlen(optarg) + 1, sizeof(char));
                if (!dest->username) {
                    fprintf(stderr, "Failed to allocate memory\n");
                    discardArgs(dest);
                    return 0;
                }

                strcpy(dest->username, optarg);
                break;
            case 'i':
                /* Borrowed from argv, like the address and the topics, rather
                   than copied like the credentials: argv outlives the run. */
                dest->clientId = optarg;
                break;
            case '?':
                if (optopt == 'h' || optopt == 'p') {
                    fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                } else if (isprint(optopt)) {
                    fprintf(stderr, "Unknown option `-%c'.\n", optopt);
                } else {
                    fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
                }

                discardArgs(dest);
                return 0;
            default:
                discardArgs(dest);
                dumpHelp();
                abort();
        }
    }

    dest->numSubscriptions = numSubscriptions;
    return 1;
}

void dumpDisplay(ProgArgs* args) {
    puts("-----");
    printf("MQTT Server Address: %s\n", args->mqttServerAddress);
    printf("MQTT Server Port:    %d\n", args->mqttServerPort);
    if (args->subscriptions != NULL) {
        char* cur = args->subscriptions[0];
        int i = 0;
        while (cur != NULL) {
            printf("Subscription:        %s\n", cur);
            cur = args->subscriptions[++i];
        }
    }
    puts("-----");
}
