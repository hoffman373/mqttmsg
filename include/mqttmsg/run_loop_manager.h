/**
 * @file run_loop_manager.h
 * @brief A cooperative run loop that paces several periodic steps.
 *
 * Register each thing that needs doing periodically with its own interval,
 * then hand control over. The manager sleeps for the shortest registered
 * interval and calls back whichever steps are due.
 *
 * @code
 * RunLoopManager* loop = initializeRunLoopManager();
 * addLoopIteration(loop, mqttRunLoopIterationRLM, mqtt, 10);
 * addLoopIteration(loop, publishStatus, mqtt, 30000);
 * mqttRunLoop(loop);            // does not return
 * @endcode
 *
 * @note Pico-only; requires the Pico SDK for its clock and sleep.
 */

#ifndef MQTTMSG_RUN_LOOP_MANAGER_H
#define MQTTMSG_RUN_LOOP_MANAGER_H
#ifdef __cplusplus
extern "C" {
#endif

#include "pico/cyw43_arch.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief One periodic step.
 * @param state The context pointer the step was registered with.
 */
typedef void (*RunLoopStep)(void *state);

/** @brief A run loop. Created by initializeRunLoopManager(). */
typedef struct RunLoopManager RunLoopManager;

/**
 * @brief Registers a step to run on an interval.
 *
 * Entries are kept sorted by interval, because mqttRunLoop() sleeps for the
 * shortest one.
 *
 * @param mgrState  Run loop to add to.
 * @param iterator  Function to call.
 * @param initState Context pointer handed back to @p iterator. Not owned.
 * @param delayMS   Minimum milliseconds between calls.
 * @return `true` if the step was registered. `false`, having logged, if the
 *         table could not be grown; the steps already registered are left
 *         intact.
 */
bool addLoopIteration(RunLoopManager *mgrState, RunLoopStep iterator, void *initState,
                      uint32_t delayMS);

/**
 * @brief Runs the loop.
 * @param mgrState Run loop to run.
 * @warning Blocks and does not return. Returns immediately, having logged
 *          an error, if no steps were registered.
 */
void mqttRunLoop(RunLoopManager *mgrState);

/**
 * @brief Creates an empty run loop.
 * @return The new run loop, or NULL if allocation failed.
 */
RunLoopManager *initializeRunLoopManager(void);

#ifdef __cplusplus
}
#endif

#endif
