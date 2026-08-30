/**
 * @file prog_args.h
 * @brief Command-line parsing for mqttlog.
 *
 * Fills in the ProgArgs that mqttSubscriberInit() wants. This is the tool's
 * own code, not the library's: a project embedding mqttmsg configures a
 * ProgArgs directly rather than from `argv`, which is why the parser lives
 * here and only the struct ships in include/.
 *
 * | Option | Meaning |
 * |---|---|
 * | `-h` | Print help and stop |
 * | `-a` | Broker address |
 * | `-p` | Broker port |
 * | `-s` | Subscription topic; repeatable |
 * | `-u` | Username |
 * | `-m` | Password |
 * | `-i` | Client identifier |
 *
 * @note Built on `getopt`.
 */

#ifndef MQTTLOG_PROG_ARGS_H
#define MQTTLOG_PROG_ARGS_H

/** @brief Broker connection settings for the host subscriber. */
typedef struct {
    const char* mqttServerAddress; /**< Broker address. Not owned. */
    short mqttServerPort;          /**< Broker TCP port. */
    char** subscriptions;          /**< Topics to subscribe to; a NULL-terminated
                                        array of pointers. Not owned. */
    int numSubscriptions;          /**< How many entries @c subscriptions holds. */
    char* username;                /**< CONNECT username, or NULL. */
    char* password;                /**< CONNECT password, or NULL. */
    /**
     * Client identifier, or NULL to generate a random one. A fixed identifier
     * is what lets a broker recognise the same client across reconnects.
     */
    const char* clientId;
} ProgArgs;


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parses arguments over built-in defaults.
 *
 * Zeroes @p dest, defaults the broker to `127.0.0.1:1883` with no
 * credentials, then parses.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param dest Receives the configuration.
 * @return 1 on success, 0 if `-h` was given or parsing failed. On 0,
 *         @p dest holds nothing that needs freeing.
 */
int mqttParseArgs(int argc, char** argv, ProgArgs* dest);

/**
 * @brief Parses arguments over defaults the caller has already set.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param dest Expected to have defaults set; parsed options overwrite them.
 * @return 1 on success, 0 if `-h` was given or parsing failed.
 * @warning On success the caller owns `dest->subscriptions`,
 *          `dest->username` and `dest->password`, and must free them.
 */
int mqttParseArgsWithDefaults(int argc, char** argv, ProgArgs* dest);

/**
 * @brief Prints the parsed configuration to stdout.
 * @param args Configuration to print. Credentials are not printed.
 */
void dumpDisplay(ProgArgs* args);

#ifdef __cplusplus
}
#endif

#endif
