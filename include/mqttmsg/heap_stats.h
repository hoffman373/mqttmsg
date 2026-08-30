/**
 * @file heap_stats.h
 * @brief Free-heap telemetry, so a slow leak shows up as a downtrend on a
 *        graph instead of a watchdog reset days later.
 *
 * Publish it on whatever status cadence a project already has, and log it
 * alongside. A healthy device holds a flat line; a leak is a straight ramp
 * down long before anything else notices.
 *
 * @note The figures are byte counts of the whole heap, not of the largest
 *       block: a fragmented heap can report plenty free and still fail an
 *       allocation.
 * @warning Nothing here allocates, but call it from the run loop rather
 *          than from an interrupt — the low-water mark is a plain
 *          read-modify-write.
 */

#ifndef MQTTMSG_HEAP_STATS_H
#define MQTTMSG_HEAP_STATS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifdef MQTTMSG_TARGET_PICO
#include <mqttmsg/mqttmsg.h>
#endif

/** @brief One sample of heap occupancy. */
typedef struct {
    /**
     * Bytes between the heap base and its ceiling: constant for a given
     * build, and the denominator if you want to publish a percentage.
     */
    uint32_t totalBytes;
    /**
     * Bytes not currently allocated: the untouched span above the break plus
     * everything on the allocator's free list.
     */
    uint32_t freeBytes;
    /**
     * Lowest #freeBytes seen since boot, or since the last
     * resetMinFreeHeapBytes(). Catches a transient spike that has already
     * been released by the time the status message goes out.
     */
    uint32_t minFreeBytes;
} HeapStats;

/**
 * @brief One consistent snapshot of all three figures.
 *
 * The low-water mark is sampled as a side effect, so calling this on the
 * status cadence is what keeps HeapStats::minFreeBytes meaningful.
 *
 * @return The snapshot.
 * @note Prefer this to the three accessors below when you want more than
 *       one figure: they each take their own sample, and on a Pico each
 *       sample briefly masks interrupts.
 */
HeapStats getHeapStats(void);

/**
 * @brief Total heap size in bytes.
 * @return HeapStats::totalBytes from a fresh sample.
 */
uint32_t getTotalHeapBytes(void);

/**
 * @brief Currently free heap in bytes.
 * @return HeapStats::freeBytes from a fresh sample.
 */
uint32_t getFreeHeapBytes(void);

/**
 * @brief Low-water mark of free heap in bytes.
 * @return HeapStats::minFreeBytes from a fresh sample.
 */
uint32_t getMinFreeHeapBytes(void);

/**
 * @brief Re-arms the low-water mark to the current free figure.
 *
 * Worth calling once after start-up allocation settles, so the mark tracks
 * steady-state behaviour rather than the boot peak.
 */
void resetMinFreeHeapBytes(void);

#ifdef MQTTMSG_TARGET_PICO
/**
 * @brief Publishes a snapshot as three retained subtopics.
 *
 * Sends `<topicPrefix>/free`, `/min_free` and `/total`, each a plain
 * decimal byte count. Retained, because the first thing anyone does with a
 * suspected leak is ask what the figure is now, and an unretained topic
 * makes them wait out a cadence. Plain integers rather than one JSON
 * document, because the consumers are Home Assistant and `mosquitto_sub`
 * and neither should need a value template to read a number.
 *
 * @param client       Session to publish on.
 * @param topicPrefix Prefix the three subtopics hang off.
 * @note This is the convenience, not the interface. For a different shape —
 *       one JSON payload, different names, extra fields — call
 *       getHeapStats() and publish it yourself.
 * @note Pico-only; compiled in when `MQTTMSG_TARGET_PICO` is defined.
 */
void publishHeapStats(MqttClient* client, const char* topicPrefix);
#endif

/**
 * @brief Total heap span reported by the platform.
 *
 * @return Bytes between the heap base and its ceiling, or 0 where there is
 *         no bounded heap to report.
 * @note Weak. A project with its own allocator, or a test that needs to
 *       script a heap, defines this and that definition wins at link time.
 *       The default reads newlib on a Pico target — the linker's `end` and
 *       `__StackLimit`. Off-target it returns 0, which makes free heap read
 *       as 0 on host builds.
 */
uint32_t mqttmsgHeapTotalBytes(void);

/**
 * @brief Allocated heap reported by the platform.
 *
 * @return Bytes currently in use, or 0 where there is nothing to report.
 * @note Weak, and overridable on the same terms as mqttmsgHeapTotalBytes().
 *       The default reads `mallinfo().uordblks`.
 */
uint32_t mqttmsgHeapUsedBytes(void);

#ifdef __cplusplus
}
#endif

#endif
