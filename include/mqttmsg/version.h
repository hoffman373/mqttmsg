/**
 * @file version.h
 * @brief The library's version, for consumers that need to compile against
 *        more than one of them.
 *
 * This header is the single source of truth: `CMakeLists.txt` parses the
 * three numbers back out of it to set `project(... VERSION)`, so there is
 * nowhere for the two to drift apart. Bump them here and CMake follows.
 *
 * Consumers pin this library by submodule SHA, which says nothing about
 * what is in it. #MQTTMSG_VERSION is an ordered integer so a feature test
 * can be written as an ordinary preprocessor comparison:
 *
 * @code
 * #include <mqttmsg/version.h>
 *
 * #if MQTTMSG_VERSION >= MQTTMSG_VERSION_ENCODE(1, 0, 0)
 *   ok = mqttSubscribe(mqtt, "device/cmd/#", onCommand, NULL);
 * #endif
 * @endcode
 *
 * @note The major number is where breaking changes land. Which release a
 *       given change belongs in is a judgement call, not something to read
 *       off the diff — these numbers are set deliberately, not bumped as a
 *       side effect of shipping something.
 */

#ifndef MQTTMSG_VERSION_H
#define MQTTMSG_VERSION_H

/** @brief Major version. Incompatible API or ABI changes. */
#define MQTTMSG_VERSION_MAJOR 1
/** @brief Minor version. Additions that keep existing code compiling. */
#define MQTTMSG_VERSION_MINOR 0
/** @brief Patch version. Fixes that change no interface. */
#define MQTTMSG_VERSION_PATCH 0

/**
 * @brief Packs a version into one comparable integer.
 *
 * Two decimal digits each for minor and patch, so the ordering only holds
 * while both stay under 100. That is a ceiling this library will not reach
 * before it would want a different scheme anyway.
 */
#define MQTTMSG_VERSION_ENCODE(major, minor, patch) ((major) * 10000 + (minor) * 100 + (patch))

/** @brief This library's version, as one comparable integer. */
#define MQTTMSG_VERSION \
    MQTTMSG_VERSION_ENCODE(MQTTMSG_VERSION_MAJOR, MQTTMSG_VERSION_MINOR, MQTTMSG_VERSION_PATCH)

/** @cond INTERNAL */
#define MQTTMSG_VERSION_STRINGIFY_(x) #x
#define MQTTMSG_VERSION_STRINGIFY(x) MQTTMSG_VERSION_STRINGIFY_(x)
/** @endcond */

/**
 * @brief This library's version as "major.minor.patch", for logging.
 *
 * Built from the three numbers above rather than written out, so it cannot
 * be left behind by a bump.
 */
#define MQTTMSG_VERSION_STRING                                                          \
    MQTTMSG_VERSION_STRINGIFY(MQTTMSG_VERSION_MAJOR)                                    \
    "." MQTTMSG_VERSION_STRINGIFY(MQTTMSG_VERSION_MINOR) "." MQTTMSG_VERSION_STRINGIFY( \
        MQTTMSG_VERSION_PATCH)

#endif
