/**
 * @file heap_stats.c
 * @brief Free-heap telemetry and its platform probes.
 *
 * The arithmetic is platform-independent and host-testable; the two weak
 * probes it rests on read newlib on a Pico and report nothing off-target.
 *
 * @see heap_stats.h
 */

#include <mqttmsg/heap_stats.h>

#ifdef MQTTMSG_TARGET_PICO
#include <malloc.h>
#include <stdio.h>

#include "hardware/sync.h"

#include <mqttmsg/logger.h>
#include <mqttmsg/mqttmsg.h>

/* Supplied by the SDK linker script (memmap_default.ld): `end` is the first
   byte past .bss, where the heap starts, and `__StackLimit` is where it
   stops. The second name is historical and misleading — on RP2350 the core
   stacks sit in scratch RAM and this is simply the heap ceiling, the same
   bound the SDK's own malloc wrapper checks against. Only the addresses
   matter; the objects are never read. */
/** @brief First byte past `.bss`, where the heap starts. */
extern char end;
/** @brief Heap ceiling. */
extern char __StackLimit;

/**
 * @brief Newlib heap span, from the linker's `end` and `__StackLimit`.
 * @return Bytes between the heap base and its ceiling.
 */
__attribute__((weak)) uint32_t mqttmsgHeapTotalBytes(void) {
    return (uint32_t)(&__StackLimit - &end);
}

/**
 * @brief Allocated bytes, from `mallinfo().uordblks`.
 * @return Bytes handed out and not yet given back.
 * @warning Masks interrupts around the call. `mallinfo()` walks the
 *          allocator's free lists and nothing else locks them: the SDK
 *          leaves `PICO_USE_MALLOC_MUTEX` off on a single-core build, while
 *          in threadsafe_background mode this library's own dispatch
 *          allocates from lwIP callbacks running in an interrupt. A walk
 *          that caught a chunk header mid-relink would chase a bad pointer.
 *          A project that replaces this probe takes on the same duty.
 */
__attribute__((weak)) uint32_t mqttmsgHeapUsedBytes(void) {
    /* uordblks is what has been handed out and not yet given back. The
       arena figure would be the easier read and the wrong one: it only ever
       grows, so once the break stops moving a leak would look like a flat
       line while the free list drains underneath it.

       Masking interrupts around the call is not paranoia about a torn integer.
       mallinfo() walks the allocator's free lists, and nothing else locks
       them: the SDK leaves PICO_USE_MALLOC_MUTEX off on a single-core build,
       while in threadsafe_background mode this library's own dispatch
       allocates from lwIP callbacks running in an interrupt. A walk that
       caught a chunk header mid-relink would chase a bad pointer, and a hard
       fault inside leak telemetry would be a poor trade. The cost is the walk
       itself, which is proportional to the number of free chunks — measured in
       microseconds, and paid on a status cadence rather than in a hot path.

       The lock lives here rather than in the caller because the interrupt that
       makes it necessary is this library's own. A project that replaces this
       probe takes on the same duty. */
    uint32_t interrupts = save_and_disable_interrupts();
    size_t used = (size_t)mallinfo().uordblks;
    restore_interrupts(interrupts);
    return (uint32_t)used;
}

#else

/*
 * Host builds (mqttlog, the test runners) have a heap the size of the
 * address space, so there is no honest figure to report.
 */
/** @brief Host stub. @return 0; there is no bounded heap to report. */
__attribute__((weak)) uint32_t mqttmsgHeapTotalBytes(void) { return 0; }
/** @brief Host stub. @return 0; there is no bounded heap to report. */
__attribute__((weak)) uint32_t mqttmsgHeapUsedBytes(void) { return 0; }

#endif

/** @brief Low-water mark of free heap, sampled by getHeapStats(). */
static uint32_t minFreeBytes = UINT32_MAX;

HeapStats getHeapStats(void) {
    HeapStats stats;
    stats.totalBytes = mqttmsgHeapTotalBytes();

    uint32_t used = mqttmsgHeapUsedBytes();
    /* Saturating, not wrapping: if a replacement probe pair ever disagrees
       about what it is counting, the reading should be a flat zero rather
       than four gigabytes of free heap on a chip with 520 KB. */
    stats.freeBytes = used < stats.totalBytes ? stats.totalBytes - used : 0;

    if (stats.freeBytes < minFreeBytes) {
        minFreeBytes = stats.freeBytes;
    }
    stats.minFreeBytes = minFreeBytes;

    return stats;
}

uint32_t getTotalHeapBytes(void) { return getHeapStats().totalBytes; }

uint32_t getFreeHeapBytes(void) { return getHeapStats().freeBytes; }

uint32_t getMinFreeHeapBytes(void) { return getHeapStats().minFreeBytes; }

void resetMinFreeHeapBytes(void) {
    minFreeBytes = UINT32_MAX;
    (void)getHeapStats();
}

#ifdef MQTTMSG_TARGET_PICO

/**
 * @brief Publishes one figure as a retained decimal payload.
 * @param client       Session to publish on.
 * @param topicPrefix Prefix the subtopic hangs off.
 * @param leaf        Subtopic name.
 * @param value       Byte count to publish.
 * @note Skips the publish entirely if the topic would be truncated:
 *       publishing to a truncated topic is worse than not publishing,
 *       because the value lands somewhere plausible-looking and wrong.
 */
static void publishHeapValue(MqttClient* client, const char* topicPrefix, const char* leaf,
                             uint32_t value) {
    char topic[128];
    char payload[16];

    int written = snprintf(topic, sizeof(topic), "%s/%s", topicPrefix, leaf);
    if (written < 0 || (size_t)written >= sizeof(topic)) {
        /* Publishing to a truncated topic is worse than not publishing: the
           value would land somewhere plausible-looking and wrong, and nobody
           watching a heap graph would think to question the topic name. */
        mqttmsgErrorPrint("heap stats: topic prefix too long, skipping (%s)\n", topicPrefix);
        return;
    }

    snprintf(payload, sizeof(payload), "%lu", (unsigned long)value);
    mqttPublish(client, topic, payload, true);
}

void publishHeapStats(MqttClient* client, const char* topicPrefix) {
    HeapStats stats = getHeapStats();

    publishHeapValue(client, topicPrefix, "free", stats.freeBytes);
    publishHeapValue(client, topicPrefix, "min_free", stats.minFreeBytes);
    publishHeapValue(client, topicPrefix, "total", stats.totalBytes);
}

#endif
