/**
 * @file run_loop_manager.c
 * @brief A cooperative run loop that paces several periodic steps.
 *
 * @see run_loop_manager.h
 */

#include <stdlib.h>

#include <mqttmsg/run_loop_manager.h>
#include <mqttmsg/logger.h>

/** @brief One registered step and its schedule. */
typedef struct RunLoopEntry {
    RunLoopStep iterator;            /**< Function to call. */
    void* state;                     /**< Context passed to @c iterator. */
    uint32_t delayMS;                /**< Minimum ms between calls. */
    absolute_time_t lastExecutionMS; /**< When @c iterator last ran. */
} RunLoopEntry;

/** @brief The registered steps, kept sorted by interval. */
struct RunLoopManager {
    RunLoopEntry* entries; /**< Steps, shortest interval first. */
    int numEntries;        /**< How many steps are registered. */
};

bool addLoopIteration(RunLoopManager* mgrState, RunLoopStep iterator, void* initState,
                      uint32_t delayMS) {
    /* Grown into a local, and published only once it holds everything, so
       running out of memory here loses the new step and nothing already
       registered. free(NULL) is fine, which is what folds the empty case
       in here. */
    RunLoopEntry* grown = calloc(1 + mgrState->numEntries, sizeof(RunLoopEntry));
    if (grown == NULL) {
        mqttmsgErrorPrint("Out of memory adding a run loop step.\n");
        return false;
    }

    for (int i = 0; i < mgrState->numEntries; i++) {
        grown[i] = mgrState->entries[i];
    }

    free(mgrState->entries);
    mgrState->entries = grown;

    mgrState->entries[mgrState->numEntries].iterator = iterator;
    mgrState->entries[mgrState->numEntries].state = initState;
    mgrState->entries[mgrState->numEntries].delayMS = delayMS;
    mgrState->numEntries++;

    // Sort entries by delayMS.
    bool hasChange = false;
    do {
        hasChange = false;

        for (int i = 0; i < mgrState->numEntries - 1; i++) {
            if (mgrState->entries[i].delayMS > mgrState->entries[i + 1].delayMS) {
                RunLoopEntry tmp = mgrState->entries[i];
                mgrState->entries[i] = mgrState->entries[i + 1];
                mgrState->entries[i + 1] = tmp;
                hasChange = true;
            }
        }
    } while (hasChange);

    return true;
}

/**
 * @brief Runs every step whose interval has elapsed.
 * @param mgrState  Run loop to service.
 * @param currentTS Timestamp to judge the intervals against.
 */
static void sharedIterationStep(RunLoopManager* mgrState, absolute_time_t currentTS) {
    for (int i = 0; i < mgrState->numEntries; i++) {
        int64_t diff = absolute_time_diff_us(mgrState->entries[i].lastExecutionMS, currentTS);
        if (diff / 1000 > mgrState->entries[i].delayMS) {
            // Time to execute.
            mgrState->entries[i].iterator(mgrState->entries[i].state);
            mgrState->entries[i].lastExecutionMS = currentTS;
        }
    }
}

void mqttRunLoop(RunLoopManager* mgrState) {
    if (mgrState->numEntries == 0) {
        mqttmsgErrorPrint("No entries provided to run.\n");
        return;
    }

    while (true) {
        sleep_ms(mgrState->entries[0].delayMS);
        absolute_time_t current = get_absolute_time();
        sharedIterationStep(mgrState, current);
    }
}

RunLoopManager* initializeRunLoopManager(void) { return calloc(1, sizeof(RunLoopManager)); }
