/* Tests for the heap telemetry (src/mqttmsg/heap_stats.c).

   The platform probes are weak, so this file defines its own pair and
   drives a scripted heap. What is worth pinning down is the arithmetic
   between the probe and the published figure: the low-water mark has to
   survive a recovery, and the subtraction must not wrap. Neither would be
   visible on a device until a graph looked wrong weeks later. */

#include "unity/unity.h"
#include "mqttmsg/heap_stats.h"

static uint32_t fakeTotal;
static uint32_t fakeUsed;

/* Strong definitions, overriding the weak defaults in heap_stats.c. */
uint32_t mqttmsgHeapTotalBytes(void) { return fakeTotal; }
uint32_t mqttmsgHeapUsedBytes(void) { return fakeUsed; }

void setUp(void) {
    fakeTotal = 200 * 1024;
    fakeUsed = 0;
    resetMinFreeHeapBytes();
}

void tearDown(void) {}

void test_total_comes_from_the_probe(void) {
    TEST_ASSERT_EQUAL_UINT32(200 * 1024, getTotalHeapBytes());
}

/* ── The snapshot ─────────────────────────────────────────────────── */

void test_snapshot_carries_all_three_figures(void) {
    fakeUsed = 40 * 1024;
    HeapStats stats = getHeapStats();
    TEST_ASSERT_EQUAL_UINT32(200 * 1024, stats.totalBytes);
    TEST_ASSERT_EQUAL_UINT32(160 * 1024, stats.freeBytes);
    TEST_ASSERT_EQUAL_UINT32(160 * 1024, stats.minFreeBytes);
}

void test_snapshot_is_internally_consistent(void) {
    /* The reason the snapshot exists: three separate accessor calls each
       take their own sample, so a heap moving underneath them can hand back
       a free figure and a mark that never coexisted. Within one snapshot
       the mark can never exceed the free figure it was taken with. */
    fakeUsed = 30 * 1024;
    (void)getHeapStats();

    fakeUsed = 120 * 1024;
    HeapStats stats = getHeapStats();

    TEST_ASSERT_EQUAL_UINT32(80 * 1024, stats.freeBytes);
    TEST_ASSERT_EQUAL_UINT32(80 * 1024, stats.minFreeBytes);
    TEST_ASSERT_TRUE(stats.minFreeBytes <= stats.freeBytes);
    TEST_ASSERT_TRUE(stats.freeBytes <= stats.totalBytes);
}

void test_snapshot_samples_the_mark(void) {
    fakeUsed = 90 * 1024;
    TEST_ASSERT_EQUAL_UINT32(110 * 1024, getHeapStats().minFreeBytes);
}

void test_accessors_agree_with_the_snapshot(void) {
    fakeUsed = 75 * 1024;
    HeapStats stats = getHeapStats();
    TEST_ASSERT_EQUAL_UINT32(stats.totalBytes, getTotalHeapBytes());
    TEST_ASSERT_EQUAL_UINT32(stats.freeBytes, getFreeHeapBytes());
    TEST_ASSERT_EQUAL_UINT32(stats.minFreeBytes, getMinFreeHeapBytes());
}

void test_free_is_total_less_in_use(void) {
    fakeUsed = 50 * 1024;
    TEST_ASSERT_EQUAL_UINT32(150 * 1024, getFreeHeapBytes());
}

void test_free_saturates_instead_of_wrapping(void) {
    /* A probe pair that disagrees about what it counts must not turn a
       full heap into 4 GB of headroom. */
    fakeUsed = fakeTotal + 1;
    TEST_ASSERT_EQUAL_UINT32(0, getFreeHeapBytes());
}

void test_min_holds_the_low_water_mark(void) {
    fakeUsed = 10 * 1024;
    (void)getFreeHeapBytes();

    fakeUsed = 180 * 1024; /* transient spike */
    (void)getFreeHeapBytes();

    fakeUsed = 10 * 1024; /* and released again */
    TEST_ASSERT_EQUAL_UINT32(190 * 1024, getFreeHeapBytes());
    TEST_ASSERT_EQUAL_UINT32(20 * 1024, getMinFreeHeapBytes());
}

void test_min_samples_rather_than_reporting_a_stale_figure(void) {
    /* Nothing has called getFreeHeapBytes() since the reset, so the mark
       can only be right if reading it takes a sample of its own. */
    fakeUsed = 60 * 1024;
    TEST_ASSERT_EQUAL_UINT32(140 * 1024, getMinFreeHeapBytes());
}

void test_min_tracks_a_steady_leak_downwards(void) {
    /* The shape the whole feature exists to make visible. */
    for (uint32_t leaked = 0; leaked <= 100 * 1024; leaked += 1024) {
        fakeUsed = leaked;
        (void)getFreeHeapBytes();
    }
    TEST_ASSERT_EQUAL_UINT32(100 * 1024, getMinFreeHeapBytes());
}

void test_reset_rearms_the_mark(void) {
    fakeUsed = 150 * 1024;
    (void)getFreeHeapBytes();
    TEST_ASSERT_EQUAL_UINT32(50 * 1024, getMinFreeHeapBytes());

    fakeUsed = 20 * 1024;
    resetMinFreeHeapBytes();
    TEST_ASSERT_EQUAL_UINT32(180 * 1024, getMinFreeHeapBytes());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_total_comes_from_the_probe);
    RUN_TEST(test_snapshot_carries_all_three_figures);
    RUN_TEST(test_snapshot_is_internally_consistent);
    RUN_TEST(test_snapshot_samples_the_mark);
    RUN_TEST(test_accessors_agree_with_the_snapshot);
    RUN_TEST(test_free_is_total_less_in_use);
    RUN_TEST(test_free_saturates_instead_of_wrapping);
    RUN_TEST(test_min_holds_the_low_water_mark);
    RUN_TEST(test_min_samples_rather_than_reporting_a_stale_figure);
    RUN_TEST(test_min_tracks_a_steady_leak_downwards);
    RUN_TEST(test_reset_rearms_the_mark);
    return UNITY_END();
}
