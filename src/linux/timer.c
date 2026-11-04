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

#include "private.h"
#include <sys/time.h>
#include <mach/mach_time.h>

#ifndef HAVE_SYS_TIMERFD_H

/* Android 4.0 does not have this header, but the kernel supports timerfds */
#ifndef SYS_timerfd_create
#ifdef __ARM_EABI__
#define __NR_timerfd_create             (__NR_SYSCALL_BASE+350)
#define __NR_timerfd_settime            (__NR_SYSCALL_BASE+353)
#define __NR_timerfd_gettime            (__NR_SYSCALL_BASE+354)
#else
#error Unsupported architecture, need to get the syscall numbers
#endif

#define SYS_timerfd_create __NR_timerfd_create
#define SYS_timerfd_settime __NR_timerfd_settime
#define SYS_timerfd_gettime __NR_timerfd_gettime
#endif /* ! SYS_timerfd_create */

/* XXX-FIXME 
   These are horrible hacks that are only known to be true on RHEL 5 x86.
 */
#ifndef SYS_timerfd_settime
#define SYS_timerfd_settime (SYS_timerfd_create + 1)
#endif 
#ifndef SYS_timerfd_gettime
#define SYS_timerfd_gettime (SYS_timerfd_create + 2)
#endif

int timerfd_create(int clockid, int flags)
{
  return syscall(SYS_timerfd_create, clockid, flags);
}

int timerfd_settime(int ufc, int flags, const struct itimerspec *utmr,
                    struct itimerspec *otmr)
{
  return syscall(SYS_timerfd_settime, ufc, flags, utmr, otmr);
}

int timerfd_gettime(int ufc, struct itimerspec *otmr)
{
  return syscall(SYS_timerfd_gettime, ufc, otmr);
}

#endif

#define LINUX_CLOCK_REALTIME 0
#define LINUX_CLOCK_MONOTONIC 1
#define LINUX_CLOCK_BOOTTIME 7
#define LINUX_TFD_TIMER_ABSTIME 1

#ifndef NOTE_MACH_CONTINUOUS_TIME
#define NOTE_MACH_CONTINUOUS_TIME 0x00000080
#endif

/* The clock a timer knote's timerfd runs on, kept in kn_flags (the timer
 * filter uses no other KNFL_ bits). CLOCK_MONOTONIC when neither is set. */
#define KNFL_TIMER_REALTIME 0x100
#define KNFL_TIMER_BOOTTIME 0x200
#define KNFL_TIMER_CLOCKS (KNFL_TIMER_REALTIME | KNFL_TIMER_BOOTTIME)

#ifndef NDEBUG
static char *
itimerspec_dump(struct itimerspec *ts)
{
    static __thread char buf[1024];

    snprintf(buf, sizeof(buf),
            "itimer: [ interval=%lu s %lu ns, next expire=%lu s %lu ns ]",
            ts->it_interval.tv_sec,
            ts->it_interval.tv_nsec,
            ts->it_value.tv_sec,
            ts->it_value.tv_nsec
           );

    return (buf);
}
#endif

static void convert_to_itimerspec(struct itimerspec *dst, uint64_t src, bool oneshot, int fflags)
{
	time_t sec, nsec;
	struct timeval now;

	if (fflags & NOTE_ABSOLUTE)
	{
		dbg_printf("%s", "...timer is absolute\n");
		oneshot = true;
		gettimeofday(&now, NULL);
	}

	// for all these NOTE_ABSOLUTE __builtin_sub_overflow checks:
	// not sure if we can just use `0` or if that's a special/reserved value, so let's use `1` to be safe

	if (fflags & NOTE_SECONDS)
	{
		dbg_printf("...timer is in seconds: %llu\n", src);
		if (fflags & NOTE_ABSOLUTE)
			if (__builtin_sub_overflow(src, now.tv_sec, &src))
				src = 1;

		sec = src;
		nsec = 0;
	}
	else if (fflags & NOTE_USECONDS)
	{
		dbg_printf("...timer is in useconds: %llu\n", src);
		if (fflags & NOTE_ABSOLUTE)
			if (__builtin_sub_overflow(src, (now.tv_sec * 1000000ull) + now.tv_usec, &src))
				src = 1;

		sec = src / 1000000;
		nsec = (src % 1000000) * 1000;
	}
	else if (fflags & NOTE_NSECONDS)
	{
		dbg_printf("...timer is in nseconds: %llu\n", src);
		if (fflags & NOTE_ABSOLUTE)
			if (__builtin_sub_overflow(src, (now.tv_sec * 1000000000ull) + now.tv_usec*1000, &src))
				src = 1;

		sec = src / 1000000000;
		nsec = src % 1000000000;
	}
	else if (fflags & NOTE_MACHTIME)
	{
		dbg_printf("...timer is in Mach-time-units: %llu\n", src);
		if (fflags & NOTE_ABSOLUTE)
			if (__builtin_sub_overflow(src, mach_absolute_time(), &src))
				src = 1;

		mach_timebase_info_data_t timebase;
		mach_timebase_info(&timebase);
		// the timebase info is in nanoseconds
		uint64_t mach_nsec = (src * timebase.numer) / (timebase.denom);
		sec = mach_nsec / 1000000000;
		nsec = mach_nsec % 1000000000;
	}
	else
	{
		dbg_printf("...timer is in mseconds: %llu\n", src);
		if (fflags & NOTE_ABSOLUTE)
			src -= (now.tv_sec * 1000) + (now.tv_usec / 1000);

		sec = src / 1000;
		nsec = (src % 1000) * 1000000;
	}

    /* Set the interval */
    if (oneshot) {
        dst->it_interval.tv_sec = 0;
        dst->it_interval.tv_nsec = 0;
    } else {
        dst->it_interval.tv_sec = sec;
        dst->it_interval.tv_nsec = nsec;
    }

    /* Set the initial expiration */
    dst->it_value.tv_sec = sec;
    dst->it_value.tv_nsec = nsec;
    dbg_printf("%s", itimerspec_dump(dst));
}

/* An absolute deadline in wall-clock units (libdispatch's walltime timers)
 * is armed on CLOCK_REALTIME at that time: converted to a relative
 * CLOCK_MONOTONIC delay, it would not count the time the machine sleeps, and
 * a phone that suspends would fire it late by as much as it slept. */
static bool timer_is_wall(int fflags)
{
	return (fflags & NOTE_ABSOLUTE) && !(fflags & NOTE_MACHTIME);
}

static int timer_clock_flags(int fflags)
{
	if (timer_is_wall(fflags))
		return KNFL_TIMER_REALTIME;
	/* Mach continuous time goes on while asleep, as CLOCK_BOOTTIME does. */
	if (fflags & NOTE_MACH_CONTINUOUS_TIME)
		return KNFL_TIMER_BOOTTIME;
	return 0;
}

static int timer_linux_clock(int clock_flags)
{
	if (clock_flags & KNFL_TIMER_REALTIME)
		return LINUX_CLOCK_REALTIME;
	if (clock_flags & KNFL_TIMER_BOOTTIME)
		return LINUX_CLOCK_BOOTTIME;
	return LINUX_CLOCK_MONOTONIC;
}

static void convert_to_wall_itimerspec(struct itimerspec *dst, uint64_t src, int fflags)
{
	uint64_t nsec;

	if (fflags & NOTE_SECONDS)
		nsec = src * 1000000000ull;
	else if (fflags & NOTE_USECONDS)
		nsec = src * 1000ull;
	else if (fflags & NOTE_NSECONDS)
		nsec = src;
	else
		nsec = src * 1000000ull;
	if (nsec == 0)
		nsec = 1; /* zero would disarm it; a past time fires at once */

	dst->it_interval.tv_sec = 0;
	dst->it_interval.tv_nsec = 0;
	dst->it_value.tv_sec = nsec / 1000000000ull;
	dst->it_value.tv_nsec = nsec % 1000000000ull;
	dbg_printf("%s", itimerspec_dump(dst));
}

/* Arms the knote's timerfd as its kevent says, first moving it to a timerfd
 * on another clock when the kevent asks for one. */
static int timer_arm(struct filter *filt, struct knote *kn)
{
	struct itimerspec ts;
	int clock_flags = timer_clock_flags(kn->kev.fflags);

	if (kn->data.pfd >= 0 && (kn->kn_flags & KNFL_TIMER_CLOCKS) != clock_flags) {
		struct epoll_event ev;
		int tfd = timerfd_create(timer_linux_clock(clock_flags), 0);

		if (tfd < 0) {
			dbg_printf("timerfd_create(2): %s", strerror(errno));
			return (-1);
		}
		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN;
		ev.data.ptr = kn;
		if (epoll_ctl(filter_epfd(filt), EPOLL_CTL_ADD, tfd, &ev) < 0) {
			dbg_printf("epoll_ctl(2): %d", errno);
			__close_for_kqueue(tfd);
			return (-1);
		}
		epoll_ctl(filter_epfd(filt), EPOLL_CTL_DEL, kn->data.pfd, NULL);
		__close_for_kqueue(kn->data.pfd);
		kn->data.pfd = tfd;
		kn->kn_flags = (kn->kn_flags & ~KNFL_TIMER_CLOCKS) | clock_flags;
	}

	if (timer_is_wall(kn->kev.fflags)) {
		convert_to_wall_itimerspec(&ts, kn->kev.data, kn->kev.fflags);
		if (timerfd_settime(kn->data.pfd, LINUX_TFD_TIMER_ABSTIME, &ts, NULL) < 0) {
			dbg_printf("timerfd_settime(2): %s", strerror(errno));
			return (-1);
		}
		return (0);
	}
	convert_to_itimerspec(&ts, kn->kev.data, kn->kev.flags & EV_ONESHOT, kn->kev.fflags);
	if (timerfd_settime(kn->data.pfd, 0, &ts, NULL) < 0) {
		dbg_printf("timerfd_settime(2): %s", strerror(errno));
		return (-1);
	}
	return (0);
}

int
evfilt_timer_copyout(struct kevent64_s *dst, struct knote *src, void *ptr)
{
    struct epoll_event * const ev = (struct epoll_event *) ptr;
    uint64_t expired;
    ssize_t n;

    kevent_int_to_64(&src->kev, dst);
    if (ev->events & EPOLLERR)
        dst->fflags = 1; /* FIXME: Return the actual timer error */
          
    /* On return, data contains the number of times the
       timer has been trigered.
     */
    n = read(src->data.pfd, &expired, sizeof(expired));
    if (n != sizeof(expired)) {
        dbg_puts("invalid read from timerfd");
        expired = 1;  /* Fail gracefully */
    } 
    dst->data = expired;

    return (0);
}

int
evfilt_timer_knote_create(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;
    struct itimerspec ts;
    int tfd;

    kn->kev.flags |= EV_CLEAR;

    int clock_flags = timer_clock_flags(kn->kev.fflags);
    tfd = timerfd_create(timer_linux_clock(clock_flags), 0);
    if (tfd < 0) {
        dbg_printf("timerfd_create(2): %s", strerror(errno));
        return (-1);
    }
    dbg_printf("created timerfd %d", tfd);
    kn->kn_flags = (kn->kn_flags & ~KNFL_TIMER_CLOCKS) | clock_flags;

    if (timer_is_wall(kn->kev.fflags)) {
        convert_to_wall_itimerspec(&ts, kn->kev.data, kn->kev.fflags);
        if (timerfd_settime(tfd, LINUX_TFD_TIMER_ABSTIME, &ts, NULL) < 0) {
            dbg_printf("timerfd_settime(2): %s", strerror(errno));
            __close_for_kqueue(tfd);
            return (-1);
        }
    } else {
        convert_to_itimerspec(&ts, kn->kev.data, kn->kev.flags & EV_ONESHOT, kn->kev.fflags);
        if (timerfd_settime(tfd, 0, &ts, NULL) < 0) {
            dbg_printf("timerfd_settime(2): %s", strerror(errno));
            __close_for_kqueue(tfd);
            return (-1);
        }
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = kn;
    if (epoll_ctl(filter_epfd(filt), EPOLL_CTL_ADD, tfd, &ev) < 0) {
        dbg_printf("epoll_ctl(2): %d", errno);
        __close_for_kqueue(tfd);
        return (-1);
    }

    kn->data.pfd = tfd;
    return (0);
}

int
evfilt_timer_knote_modify(struct filter *filt, struct knote *kn, 
        const struct kevent64_s *kev)
{
    bool was_disabled = kn->kev.flags & EV_DISABLE;

    kn->kev.data = kev->data;
    kn->kev.flags = kev->flags;
    kn->kev.fflags = kev->fflags;

    /* Updating a disabled timer must not arm it. If requested, the common
     * enable transition will arm the updated configuration afterward. */
    if (was_disabled)
        return (0);

    if (!kn->kev.data)
        kn->kev.data = 1;

    return (timer_arm(filt, kn));
}

int
evfilt_timer_knote_delete(struct filter *filt, struct knote *kn)
{
    int rv = 0;

    if (kn->data.pfd == -1)
        return (0);

    if (epoll_ctl(filter_epfd(filt), EPOLL_CTL_DEL, kn->data.pfd, NULL) < 0) {
        dbg_printf("epoll_ctl(2): %s", strerror(errno));
        rv = -1;
    }
    if (__close_for_kqueue(kn->data.pfd) < 0) {
        dbg_printf("close(2): %s", strerror(errno));
        rv = -1;
    }

    kn->data.pfd = -1;
    return (rv);
}

int
evfilt_timer_knote_enable(struct filter *filt, struct knote *kn)
{
    if (!kn->kev.data)
        kn->kev.data = 1;

    return (timer_arm(filt, kn));
}

int
evfilt_timer_knote_disable(struct filter *filt, struct knote *kn)
{
    struct itimerspec ts;
    int tfd = kn->data.pfd;

    memset(&ts, 0, sizeof(ts));

    if (timerfd_settime(tfd, 0, &ts, NULL) < 0) {
        dbg_printf("timerfd_settime(2): %s", strerror(errno));
        return (-1);
    }

    return 0;
}

const struct filter evfilt_timer = {
    EVFILT_TIMER,
    NULL,
    NULL,
    evfilt_timer_copyout,
    evfilt_timer_knote_create,
    evfilt_timer_knote_modify,
    evfilt_timer_knote_delete,
    evfilt_timer_knote_enable,
    evfilt_timer_knote_disable,     
};
