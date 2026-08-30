/**
 * @file logger.h
 * @brief Compile-time gated printf logging and hex dumps.
 *
 * Each level is a macro that expands to a `printf` guarded by a constant,
 * so a disabled level costs nothing at runtime and the compiler discards
 * the call and its arguments. Set the levels from CMake:
 *
 * @code
 * target_compile_definitions(mqttmsg INTERFACE MQTTMSG_DEBUG=1 MQTTMSG_TRACE=0)
 * @endcode
 *
 * @note Errors are on by default; debug and trace are off.
 */

#ifndef MQTTMSG_LOGGER_H
#define MQTTMSG_LOGGER_H

#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>

#include <mqttmsg/mqtt_payload.h>

/** @brief Set non-zero to enable #mqttmsgDebugPrint. Off by default. */
#ifndef MQTTMSG_DEBUG
#define MQTTMSG_DEBUG 0
#endif

/** @brief Set non-zero to enable #mqttmsgTracePrint and #MQTTMSG_DUMP_BYTES. Off by default. */
#ifndef MQTTMSG_TRACE
#define MQTTMSG_TRACE 0
#endif

/** @brief Set zero to silence #mqttmsgErrorPrint. On by default. */
#ifndef MQTTMSG_ERROR
#define MQTTMSG_ERROR 1
#endif

/**
 * @def mqttmsgDebugPrint
 * @brief Logs at debug level when #MQTTMSG_DEBUG is non-zero.
 * @param fmt printf format string, followed by its arguments.
 */
#define mqttmsgDebugPrint(fmt, ...)     \
    do {                                \
        if (MQTTMSG_DEBUG) {            \
            printf(fmt, ##__VA_ARGS__); \
        }                               \
    } while (0)

/**
 * @def mqttmsgTracePrint
 * @brief Logs at trace level when #MQTTMSG_TRACE is non-zero.
 * @param fmt printf format string, followed by its arguments.
 */
#define mqttmsgTracePrint(fmt, ...)     \
    do {                                \
        if (MQTTMSG_TRACE) {            \
            printf(fmt, ##__VA_ARGS__); \
        }                               \
    } while (0)

/**
 * @def mqttmsgErrorPrint
 * @brief Logs at error level unless #MQTTMSG_ERROR is zero.
 * @param fmt printf format string, followed by its arguments.
 */
#define mqttmsgErrorPrint(fmt, ...)     \
    do {                                \
        if (MQTTMSG_ERROR) {            \
            printf(fmt, ##__VA_ARGS__); \
        }                               \
    } while (0)

/**
 * @brief Prints a buffer as hex, 16 bytes to a line.
 * @param bptr Bytes to dump, or NULL to print that there were none.
 * @param len  How many bytes to dump.
 */
static inline void mqttmsgDumpBytes(const uint8_t *bptr, uint32_t len) {
    unsigned int i = 0;

    if (bptr == NULL) {
        printf("mqttmsgDumpBytes: no bytes\n");
        return;
    }

    /* PRIu32 rather than %u: uint32_t is int on the host and long on the
       RP2350, so a literal %u is wrong on exactly the target that matters. */
    printf("mqttmsgDumpBytes %" PRIu32, len);
    for (i = 0; i < len;) {
        if ((i & 0x0f) == 0) {
            printf("\n");
        } else if ((i & 0x07) == 0) {
            printf(" ");
        }
        printf("%02x ", bptr[i++]);
    }
    printf("\n");
}

static inline void mqttmsgPrintAsciiLine(const uint8_t *bptr, int pos, int len);

/**
 * @brief Prints a buffer as hex with an ASCII column beside it.
 *
 * A short final row is padded with `XX` so the ASCII column stays aligned.
 *
 * @param bptr Bytes to dump, or NULL to print that there were none.
 * @param len  How many bytes to dump.
 */
static inline void mqttmsgDumpBytesAscii(const uint8_t *bptr, uint32_t len) {
    unsigned int i = 0;

    if (bptr == NULL) {
        printf("mqttmsgDumpBytesAscii: no bytes\n");
        return;
    }

    printf("mqttmsgDumpBytesAscii %" PRIu32 "\n", len);
    for (i = 0; i < len;) {
        if (i > 0 && (i & 0x0f) == 0) {
            // Need to reprint the previous 16 bytes in ascii now.
            mqttmsgPrintAsciiLine(bptr, i - 16, 16);

            printf("\n");
        } else if (i > 0 && (i & 0x07) == 0) {
            printf(" ");
        }
        printf("%02x ", bptr[i++]);
    }

    int remaining = (int)(len % 16);
    if (remaining > 0) {
        for (int z = 0; z < 16 - remaining; z++) {
            if ((z + remaining) == 8) {
                printf(" ");
            }

            printf("XX ");
        }

        // Print the remainder of the last row.
        mqttmsgPrintAsciiLine(bptr, len - remaining, remaining);
    }

    printf("\n");
}

/**
 * @brief Prints one row of the ASCII column for mqttmsgDumpBytesAscii().
 * @param bptr Buffer being dumped.
 * @param pos  Offset of the first byte of the row.
 * @param len  How many bytes the row covers.
 */
static inline void mqttmsgPrintAsciiLine(const uint8_t *bptr, int pos, int len) {
    for (int i = pos; i < pos + len; i++) {
        if ((i & 0x07) == 0) {
            printf(" ");
        }

        printf("%c ", bptr[i]);
    }
}

/**
 * @brief Prints why there is nothing to dump, for a MqttPayload that is not Ok.
 * @param what Name of the caller, for the line it prints.
 * @param p    MqttPayload to describe.
 * @return true if @p p had no bytes, and the caller should stop.
 */
static inline bool mqttmsgPrintAbsentPayload(const char *what, MqttPayload p) {
    if (mqttPayloadIsOk(p)) {
        return false;
    }

    if (mqttPayloadIsFailure(p)) {
        printf("%s: nothing built, failure %08x\n", what, (unsigned)mqttPayloadReason(p));
    } else {
        printf("%s: no body\n", what);
    }

    return true;
}

/**
 * @brief Prints a MqttPayload as hex, 16 bytes to a line.
 *
 * The form to reach for over mqttmsgDumpBytes(): a MqttPayload with no body, or one
 * that failed to build, says so instead of every call site having to ask
 * first.
 *
 * @param p MqttPayload to dump.
 */
static inline void mqttmsgDumpPayload(MqttPayload p) {
    if (mqttmsgPrintAbsentPayload("mqttmsgDumpPayload", p)) {
        return;
    }

    mqttmsgDumpBytes(mqttPayloadBytes(p), mqttPayloadLength(p));
}

/**
 * @brief Prints a MqttPayload as hex with an ASCII column beside it.
 * @param p MqttPayload to dump.
 * @see mqttmsgDumpPayload
 */
static inline void mqttmsgDumpPayloadAscii(MqttPayload p) {
    if (mqttmsgPrintAbsentPayload("mqttmsgDumpPayloadAscii", p)) {
        return;
    }

    mqttmsgDumpBytesAscii(mqttPayloadBytes(p), mqttPayloadLength(p));
}

/**
 * @def MQTTMSG_DUMP_BYTES
 * @brief Calls mqttmsgDumpBytes() when #MQTTMSG_TRACE is non-zero, and expands to nothing
 *        otherwise.
 * @param A Bytes to dump.
 * @param B How many bytes to dump.
 */
#if MQTTMSG_TRACE
#define MQTTMSG_DUMP_BYTES mqttmsgDumpBytes
#else
#define MQTTMSG_DUMP_BYTES(A, B)
#endif

/**
 * @def MQTTMSG_DUMP_PAYLOAD
 * @brief Calls mqttmsgDumpPayload() when #MQTTMSG_TRACE is non-zero, and expands to
 *        nothing otherwise.
 * @param P MqttPayload to dump.
 */
/**
 * @def MQTTMSG_DUMP_PAYLOAD_ASCII
 * @brief Calls mqttmsgDumpPayloadAscii() when #MQTTMSG_TRACE is non-zero, and expands to
 *        nothing otherwise.
 * @param P MqttPayload to dump.
 */
#if MQTTMSG_TRACE
#define MQTTMSG_DUMP_PAYLOAD mqttmsgDumpPayload
#define MQTTMSG_DUMP_PAYLOAD_ASCII mqttmsgDumpPayloadAscii
#else
#define MQTTMSG_DUMP_PAYLOAD(P)
#define MQTTMSG_DUMP_PAYLOAD_ASCII(P)
#endif

#endif
