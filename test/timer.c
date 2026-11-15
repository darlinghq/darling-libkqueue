/*
 * Copyright (c) 2009 Mark Heily <mark@heily.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "common.h"

#include <time.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#ifndef NOTE_MACH_CONTINUOUS_TIME
#define NOTE_MACH_CONTINUOUS_TIME 0x00000080
#endif
#endif

void
test_kevent_timer_add(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_TIMER, EV_ADD, 0, 1000, NULL);
}

void
test_kevent_timer_del(struct test_context *ctx)
{
    struct kevent kev;

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);

    test_no_kevents(ctx->kqfd);
}

void
test_kevent_timer_get(struct test_context *ctx)
{
    struct kevent kev, ret;

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_TIMER, EV_ADD, 0, 1000, NULL);

    kev.flags |= EV_CLEAR;
    kev.data = 1; 
    kevent_get(&ret, ctx->kqfd);
    kevent_cmp(&kev, &ret);

    kevent_add(ctx->kqfd, &kev, 1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
}

static void
test_kevent_timer_oneshot(struct test_context *ctx)
{
    struct kevent kev, ret;

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 2, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 500,NULL);

    /* Retrieve the event */
    kev.flags = EV_ADD | EV_CLEAR | EV_ONESHOT;
    kev.data = 1; 
    kevent_get(&ret, ctx->kqfd);
    kevent_cmp(&kev, &ret);

    /* Check if the event occurs again */
    sleep(3);
    test_no_kevents(ctx->kqfd);
}

static void
test_kevent_timer_periodic(struct test_context *ctx)
{
    struct kevent kev, ret;

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 3, EVFILT_TIMER, EV_ADD, 0, 1000,NULL);

    /* Retrieve the event */
    kev.flags = EV_ADD | EV_CLEAR;
    kev.data = 1; 
    kevent_get(&ret, ctx->kqfd);
    kevent_cmp(&kev, &ret);

    /* Check if the event occurs again */
    sleep(1);
    kevent_get(&ret, ctx->kqfd);
    kevent_cmp(&kev, &ret);

    /* Delete the event */
    kev.flags = EV_DELETE;
    kevent_update(ctx->kqfd, &kev);
}

static void
test_kevent_timer_disable_and_enable(struct test_context *ctx)
{
    struct kevent kev, ret;

    test_no_kevents(ctx->kqfd);

    /* Add the watch and immediately disable it */
    kevent_add(ctx->kqfd, &kev, 4, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 2000,NULL);
    kev.flags = EV_DISABLE;
    kevent_update(ctx->kqfd, &kev);
    test_no_kevents(ctx->kqfd);

    /* Re-enable and check again */
    kev.flags = EV_ENABLE;
    kevent_update(ctx->kqfd, &kev);

    kev.flags = EV_ADD | EV_CLEAR | EV_ONESHOT;
    kev.data = 1; 
    kevent_get(&ret, ctx->kqfd);
    kevent_cmp(&kev, &ret);
}

#ifdef EV_DISPATCH
void
test_kevent_timer_dispatch(struct test_context *ctx)
{
    struct kevent kev, ret;

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 4, EVFILT_TIMER, EV_ADD | EV_DISPATCH, 0, 800, NULL);

    /* Get one event */
    kev.flags = EV_ADD | EV_CLEAR | EV_DISPATCH;
    kev.data = 1; 
    kevent_get(&ret, ctx->kqfd);
    kevent_cmp(&kev, &ret);

    /* Confirm that the knote is disabled */
    sleep(1);
    test_no_kevents(ctx->kqfd);

    /* Enable the knote and make sure no events are pending */
    kevent_add(ctx->kqfd, &kev, 4, EVFILT_TIMER, EV_ENABLE | EV_DISPATCH, 0, 800, NULL);
    test_no_kevents(ctx->kqfd);

    /* Get the next event */
    sleep(1);
    kev.flags = EV_ADD | EV_CLEAR | EV_DISPATCH;
    kev.data = 1; 
    kevent_get(&ret, ctx->kqfd);
    kevent_cmp(&kev, &ret);

    /* Remove the knote and ensure the event no longer fires */
    kevent_add(ctx->kqfd, &kev, 4, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    sleep(1);
    test_no_kevents(ctx->kqfd);
}
#endif  /* EV_DISPATCH */

/*
 * NOTE_ABSOLUTE timers. Expectations are what macOS does (observed on 26.7.3):
 * with a unit flag (or none, milliseconds) the deadline is a wall-clock time
 * since the epoch; with NOTE_MACHTIME it is a mach_absolute_time() value, or a
 * mach_continuous_time() one together with NOTE_MACH_CONTINUOUS_TIME. A deadline
 * in the past fires at once. NOTE_SECONDS truncates "now" to whole seconds, so
 * such a timer may fire up to a second early.
 */
#define TIMER_ABS_FAIL(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

static double
timer_abs_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Waits up to timeout seconds for one event of the timer; returns the seconds waited, or -1 on timeout. */
static double
timer_abs_wait(int kqfd, uintptr_t ident, double timeout)
{
    struct kevent ret;
    struct timespec ts;
    double start = timer_abs_now();
    int n;

    ts.tv_sec = (time_t) timeout;
    ts.tv_nsec = (long) ((timeout - (double) ts.tv_sec) * 1e9);
    n = kevent(kqfd, NULL, 0, &ret, 1, &ts);
    if (n < 0)
        TIMER_ABS_FAIL("kevent: %s", strerror(errno));
    if (n == 0)
        return (-1);
    if (ret.filter != EVFILT_TIMER || ret.ident != ident || ret.data != 1)
        TIMER_ABS_FAIL("unexpected event: ident %lu data %ld", (unsigned long) ret.ident, (long) ret.data);
    return (timer_abs_now() - start);
}

/* now (wall clock) plus offset seconds, in nanoseconds since the epoch */
static int64_t
timer_abs_wall_ns(double offset)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec + (int64_t) (offset * 1e9);
}

static void
timer_abs_expect(struct test_context *ctx, const char *label, u_int fflags, int64_t data, double min, double max)
{
    struct kevent kev;
    double took;

    kevent_add(ctx->kqfd, &kev, 9, EVFILT_TIMER, EV_ADD | EV_ONESHOT, fflags, data, NULL);
    took = timer_abs_wait(ctx->kqfd, 9, max + 1.0);
    if (took < 0 || took < min || took > max)
        TIMER_ABS_FAIL("%s: fired after %.2f s, expected between %.2f and %.2f s", label, took, min, max);
}

static void
test_kevent_timer_absolute_wall(struct test_context *ctx)
{
    int64_t ns;

    test_no_kevents(ctx->kqfd);

    ns = timer_abs_wall_ns(1.5);
    timer_abs_expect(ctx, "microseconds", NOTE_ABSOLUTE | NOTE_USECONDS, ns / 1000, 1.2, 2.0);
    ns = timer_abs_wall_ns(1.5);
    timer_abs_expect(ctx, "nanoseconds", NOTE_ABSOLUTE | NOTE_NSECONDS, ns, 1.2, 2.0);
    ns = timer_abs_wall_ns(1.5);
    timer_abs_expect(ctx, "milliseconds", NOTE_ABSOLUTE, ns / 1000000, 1.2, 2.0);
    ns = timer_abs_wall_ns(2.0);
    timer_abs_expect(ctx, "seconds", NOTE_ABSOLUTE | NOTE_SECONDS, ns / 1000000000, 0.9, 2.5);

    /* a deadline in the past, or zero, fires at once */
    ns = timer_abs_wall_ns(-10.0);
    timer_abs_expect(ctx, "past microseconds", NOTE_ABSOLUTE | NOTE_USECONDS, ns / 1000, 0.0, 0.5);
    timer_abs_expect(ctx, "past nanoseconds", NOTE_ABSOLUTE | NOTE_NSECONDS, ns, 0.0, 0.5);
    timer_abs_expect(ctx, "past milliseconds", NOTE_ABSOLUTE, ns / 1000000, 0.0, 0.5);
    timer_abs_expect(ctx, "past seconds", NOTE_ABSOLUTE | NOTE_SECONDS, ns / 1000000000, 0.0, 0.5);
    timer_abs_expect(ctx, "zero", NOTE_ABSOLUTE | NOTE_USECONDS, 0, 0.0, 0.5);
}

#ifdef __APPLE__
static void
test_kevent_timer_absolute_mach(struct test_context *ctx)
{
    mach_timebase_info_data_t tb;
    uint64_t one_second;

    test_no_kevents(ctx->kqfd);
    mach_timebase_info(&tb);
    one_second = 1000000000ULL * tb.denom / tb.numer;

    timer_abs_expect(ctx, "mach time", NOTE_ABSOLUTE | NOTE_MACHTIME, mach_absolute_time() + one_second, 0.8, 1.5);
    timer_abs_expect(ctx, "past mach time", NOTE_ABSOLUTE | NOTE_MACHTIME, mach_absolute_time() - 10 * one_second, 0.0, 0.5);
    timer_abs_expect(ctx, "continuous mach time", NOTE_ABSOLUTE | NOTE_MACHTIME | NOTE_MACH_CONTINUOUS_TIME,
        mach_continuous_time() + one_second, 0.8, 1.5);
    timer_abs_expect(ctx, "past continuous mach time", NOTE_ABSOLUTE | NOTE_MACHTIME | NOTE_MACH_CONTINUOUS_TIME,
        mach_continuous_time() - 10 * one_second, 0.0, 0.5);
    timer_abs_expect(ctx, "relative continuous", NOTE_MACH_CONTINUOUS_TIME, 300, 0.2, 0.7);
    timer_abs_expect(ctx, "relative continuous microseconds", NOTE_MACH_CONTINUOUS_TIME | NOTE_USECONDS, 300000, 0.2, 0.7);
}

#endif /* __APPLE__ */

/* An absolute timer re-added with a new deadline fires at the new one; a disabled one does not fire until enabled. */
static void
test_kevent_timer_absolute_rearm(struct test_context *ctx)
{
    struct kevent kev;
    double took;
    int64_t deadline;

    test_no_kevents(ctx->kqfd);

    kevent_add(ctx->kqfd, &kev, 9, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_ABSOLUTE | NOTE_USECONDS,
        timer_abs_wall_ns(30.0) / 1000, NULL);
    kevent_add(ctx->kqfd, &kev, 9, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_ABSOLUTE | NOTE_USECONDS,
        timer_abs_wall_ns(0.4) / 1000, NULL);
    took = timer_abs_wait(ctx->kqfd, 9, 3.0);
    if (took < 0.2 || took > 1.0)
        TIMER_ABS_FAIL("re-armed absolute timer fired after %.2f s", took);

    deadline = timer_abs_wall_ns(0.3) / 1000;
    kevent_add(ctx->kqfd, &kev, 10, EVFILT_TIMER, EV_ADD | EV_DISABLE | EV_ONESHOT, NOTE_ABSOLUTE | NOTE_USECONDS,
        deadline, NULL);
    if (timer_abs_wait(ctx->kqfd, 10, 0.8) >= 0)
        TIMER_ABS_FAIL("a disabled timer fired");
    kevent_add(ctx->kqfd, &kev, 10, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_ABSOLUTE | NOTE_USECONDS,
        deadline, NULL);
    if (timer_abs_wait(ctx->kqfd, 10, 2.0) < 0)
        TIMER_ABS_FAIL("an enabled absolute timer whose time has passed did not fire");
}

void
test_evfilt_timer(struct test_context *ctx)
{
    test(kevent_timer_add, ctx);
    test(kevent_timer_del, ctx);
    test(kevent_timer_get, ctx);
    test(kevent_timer_oneshot, ctx);
    test(kevent_timer_periodic, ctx);
    test(kevent_timer_disable_and_enable, ctx);
    test(kevent_timer_absolute_wall, ctx);
#ifdef __APPLE__
    test(kevent_timer_absolute_mach, ctx);
#endif
    test(kevent_timer_absolute_rearm, ctx);
#ifdef EV_DISPATCH
    test(kevent_timer_dispatch, ctx);
#endif
}
