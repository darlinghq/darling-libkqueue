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

#include <limits.h>

#include "private.h"

#ifdef DARLING
#include <darling/emulation/linux_premigration/ext/for-libkqueue.h>
#endif

#if HAVE_SYS_SIGNALFD_H
# include <sys/signalfd.h>
#else
#define signalfd(a,b,c) syscall(SYS_signalfd, (a), (b), (c))
#define SFD_NONBLOCK 04000
struct signalfd_siginfo
{
  uint32_t ssi_signo;
  int32_t ssi_errno;
  int32_t ssi_code;
  uint32_t ssi_pid;
  uint32_t ssi_uid;
  int32_t ssi_fd;
  uint32_t ssi_tid;
  uint32_t ssi_band;
  uint32_t ssi_overrun;
  uint32_t ssi_trapno;
  int32_t ssi_status;
  int32_t ssi_int;
  uint64_t ssi_ptr;
  uint64_t ssi_utime;
  uint64_t ssi_stime;
  uint64_t ssi_addr;
  uint8_t __pad[48];
};
#endif

/* Discards the pending signals and returns how many there were. */
static int
signalfd_reset(int sigfd)
{
    struct signalfd_siginfo sig;
    ssize_t n;
    int count = 0;

    for (;;) {
        n = read(sigfd, &sig, sizeof(sig));
        if (n == sizeof(sig)) {
            count++;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EWOULDBLOCK)
            return (count);
        dbg_perror("read(2) from signalfd");
        abort();
    }
}

#ifdef DARLING
/*
 * A signal filter records every attempt to deliver the signal to the
 * process, whatever its disposition. The signalfd only sees a signal that
 * stays pending because every thread blocks it; one that a thread takes
 * (even to ignore it) is counted on this eventfd instead.
 */
static int
sigeventfd_create(int epfd, void *ptr, int signum)
{
    struct epoll_event ev;
    int evfd;
    int rv;

    evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evfd < 0) {
        dbg_perror("eventfd(2)");
        return (-1);
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = ptr;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, evfd, &ev) < 0) {
        dbg_perror("epoll_ctl(2)");
        (void) __close_for_kqueue(evfd);
        return (-1);
    }

    rv = _signal_watch_4libkqueue(signum, evfd);
    if (rv < 0) {
        dbg_printf("cannot watch signal %d: %d", signum, rv);
        (void) epoll_ctl(epfd, EPOLL_CTL_DEL, evfd, NULL);
        (void) __close_for_kqueue(evfd);
        return (-1);
    }

    return (evfd);
}

static void
sigeventfd_delete(int epfd, int evfd, int signum)
{
    _signal_unwatch_4libkqueue(signum, evfd);
    if (epfd >= 0)
        (void) epoll_ctl(epfd, EPOLL_CTL_DEL, evfd, NULL);
    (void) __close_for_kqueue(evfd);
}

/* Returns how many signals were counted since the last call. */
static int
sigeventfd_reset(int evfd)
{
    uint64_t count;
    ssize_t n;

    do {
        n = read(evfd, &count, sizeof(count));
    } while (n < 0 && errno == EINTR);

    if (n != sizeof(count))
        return (0);
    return (count > INT_MAX) ? INT_MAX : (int) count;
}
#endif

static int
signalfd_add(int epfd, int sigfd, void *ptr)
{
    struct epoll_event ev;
    int rv;

    /* Add the signalfd to the kqueue's epoll descriptor set */
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = ptr;
    rv = epoll_ctl(epfd, EPOLL_CTL_ADD, sigfd, &ev);
    if (rv < 0) {
        dbg_perror("epoll_ctl(2)");
        return (-1);
    }

    return (0);
}

static int
signalfd_create(int epfd, void *ptr, int signum)
{
    static int flags = SFD_NONBLOCK;
    sigset_t sigmask;
    int sigfd;

    /* Create a signalfd */
    sigemptyset(&sigmask);
    sigaddset(&sigmask, signum);
    sigfd = signalfd(-1, &sigmask, flags);

    /* WORKAROUND: Flags are broken on kernels older than Linux 2.6.27 */
    if (sigfd < 0 && errno == EINVAL && flags != 0) {
        flags = 0;
        sigfd = signalfd(-1, &sigmask, flags);
    }
    if (sigfd < 0) {
        dbg_perror("signalfd(2)");
        goto errout;
    }
    fcntl(sigfd, F_SETFD, FD_CLOEXEC);

    /* Block the signal handler from being invoked */
    if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0) {
        dbg_perror("sigprocmask(2)");
        goto errout;
    }

    signalfd_reset(sigfd);

    if (signalfd_add(epfd, sigfd, ptr) < 0)
        goto errout;

    dbg_printf("added sigfd %d to epfd %d (signum=%d)", sigfd, epfd, signum);

    return (sigfd);

errout:
    (void) __close_for_kqueue(sigfd);
    return (-1);
}

int
evfilt_signal_copyout(struct kevent64_s *dst, struct knote *src, void *x UNUSED)
{
    int count;

    count = signalfd_reset(src->kdata.kn_signalfd);
#ifdef DARLING
    if (src->kn_sigeventfd >= 0)
        count += sigeventfd_reset(src->kn_sigeventfd);
#endif

    kevent_int_to_64(&src->kev, dst);
    if (count == 0) {
        /* Both descriptors were ready and the other one took the count. */
        dst->filter = EVFILT_DROP;
        return (0);
    }
    /* The number of times the signal occurred since the last report. */
    dst->data = count;

    return (0);
}

int
evfilt_signal_knote_create(struct filter *filt, struct knote *kn)
{
    int fd;

    kn->kn_sigeventfd = -1;
    fd = signalfd_create(filter_epfd(filt), kn, kn->kev.ident);
    if (fd > 0) {
        kn->kev.flags |= EV_CLEAR;
        kn->kdata.kn_signalfd = fd;
#ifdef DARLING
        kn->kn_sigeventfd = sigeventfd_create(filter_epfd(filt), kn, kn->kev.ident);
        if (kn->kn_sigeventfd < 0) {
            (void) epoll_ctl(filter_epfd(filt), EPOLL_CTL_DEL, fd, NULL);
            (void) __close_for_kqueue(fd);
            kn->kdata.kn_signalfd = -1;
            return (-1);
        }
#endif
        return (0);
    } else {
        kn->kdata.kn_signalfd = -1;
        return (-1);
    }
}

int
evfilt_signal_knote_modify(struct filter *filt UNUSED, 
        struct knote *kn UNUSED, 
        const struct kevent64_s *kev UNUSED)
{
    /* Nothing to do since the signal number does not change. */

    return (0);
}

int
evfilt_signal_knote_delete(struct filter *filt, struct knote *kn)
{
    const int sigfd = kn->kdata.kn_signalfd;

    /* Needed so that delete() can be called after disable() */
    if (kn->kdata.kn_signalfd == -1)
        return (0);

#ifdef DARLING
    /* Stop counting first: after a fork, the epoll set below is gone. */
    if (kn->kn_sigeventfd >= 0) {
        sigeventfd_delete(filter_epfd(filt), kn->kn_sigeventfd, kn->kev.ident);
        kn->kn_sigeventfd = -1;
    }
#endif

    if (epoll_ctl(filter_epfd(filt), EPOLL_CTL_DEL, sigfd, NULL) < 0) {
        dbg_perror("epoll_ctl(2)");
        return (-1);
    }

    if (__close_for_kqueue(sigfd) < 0) {
        dbg_perror("close(2)");
        return (-1);
    }

    /* NOTE: This does not call sigprocmask(3) to unblock the signal. */
    kn->kdata.kn_signalfd = -1;

    return (0);
}

int
evfilt_signal_knote_enable(struct filter *filt, struct knote *kn)
{
    dbg_printf("enabling ident %u", (unsigned int) kn->kev.ident);
    return evfilt_signal_knote_create(filt, kn);
}

int
evfilt_signal_knote_disable(struct filter *filt, struct knote *kn)
{
    dbg_printf("disabling ident %u", (unsigned int) kn->kev.ident);
    return evfilt_signal_knote_delete(filt, kn);
}


const struct filter evfilt_signal = {
    EVFILT_SIGNAL,
    NULL,
    NULL,
    evfilt_signal_copyout,
    evfilt_signal_knote_create,
    evfilt_signal_knote_modify,
    evfilt_signal_knote_delete,
    evfilt_signal_knote_enable,
    evfilt_signal_knote_disable,         
};
