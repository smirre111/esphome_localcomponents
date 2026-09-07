// A wall clock built on CLOCK_MONOTONIC.
//
// Production reads gettimeofday(); the schedule tests read time(). Neither can
// be stepped under host test — there is no settimeofday shim — so those tests
// build their entries relative to now, sleep, and read the clock again. That
// is correct only as long as the machine's wall clock moves forward at a
// steady rate.
//
// On a machine whose clock is corrected mid-run it does not. Observed here: a
// container clock repeatedly snapping back three weeks made six schedule tests
// fail in a parallel run and pass when re-run alone, because an entry armed
// "one minute ago" arrived from the future. The failure reads like a logic bug
// and is not one.
//
// So both calls are interposed and answered as
//
//     epoch_at_start + (CLOCK_MONOTONIC now - CLOCK_MONOTONIC at start)
//
// CLOCK_MONOTONIC is not affected by settimeofday or NTP steps, so an interval
// measured across a sleep is the interval that elapsed, whatever the host's
// wall clock did meanwhile. The absolute value is still a real epoch — the
// tests need that, since they derive minute-of-day from it — it is simply
// anchored once instead of re-read.
//
// This is NOT the settable clock that Mode C's window tests want (test-plan.md
// section 3): time cannot be stepped forward here, only observed. It is the
// smaller thing — it removes an environment failure mode without changing any
// test's semantics.

#define _GNU_SOURCE
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

static int64_t g_epoch_base_us = 0;   // wall clock at first use
static int64_t g_mono_base_us  = 0;   // monotonic clock at that same moment
static int     g_anchored      = 0;

static int64_t read_clock_us(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int64_t now_us(void) {
    if (!g_anchored) {
        g_epoch_base_us = read_clock_us(CLOCK_REALTIME);
        g_mono_base_us  = read_clock_us(CLOCK_MONOTONIC);
        g_anchored      = 1;
    }
    return g_epoch_base_us + (read_clock_us(CLOCK_MONOTONIC) - g_mono_base_us);
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void) tz;
    const int64_t us = now_us();
    if (tv) {
        tv->tv_sec  = (time_t) (us / 1000000);
        tv->tv_usec = (suseconds_t) (us % 1000000);
    }
    return 0;
}

time_t time(time_t *out) {
    const time_t s = (time_t) (now_us() / 1000000);
    if (out) *out = s;
    return s;
}
