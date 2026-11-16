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

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include <darling/emulation/linux_premigration/ext/for-libkqueue.h>

#include "private.h"

/*
 * Capacity of a pipe's buffer. Darwin's fcntl(2) has no command to query or
 * change it, so every pipe a Darwin program can see has the system default.
 */
#define PIPE_BUFFER_SIZE 65536

/*
 * Space available in the write buffer of fd, never negative. For a socket
 * this is the send buffer size less the bytes still queued (TIOCOUTQ alone
 * reports only the queued bytes, 0 for an idle connection); for a pipe the
 * buffer capacity less the unread bytes. Other descriptors keep reporting
 * the queued byte count.
 */
static int64_t
write_space_avail(int fd)
{
    struct stat sb;
    int queued = 0, sndbuf = 0;
    socklen_t sl = sizeof(sndbuf);

    if (fstat(fd, &sb) < 0) {
        /* race condition with close, so ignore this error */
        return (0);
    }

    if (S_ISFIFO(sb.st_mode)) {
        if (ioctl(fd, FIONREAD, &queued) < 0)
            return (0);
        return (queued < PIPE_BUFFER_SIZE) ? (PIPE_BUFFER_SIZE - queued) : 0;
    }

    if (ioctl(fd, TIOCOUTQ, &queued) < 0) {
        /* race condition with socket close, so ignore this error */
        dbg_puts("ioctl(2) of socket failed");
        return (0);
    }
    if (!S_ISSOCK(sb.st_mode))
        return (queued);

    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &sl) < 0)
        return (0);
    return (sndbuf > queued) ? (sndbuf - queued) : 0;
}

int
evfilt_socket_copyout(struct kevent64_s *dst, struct knote *src, void *ptr)
{
    struct epoll_event * const ev = (struct epoll_event *) ptr;

    epoll_event_dump(ev);
    kevent_int_to_64(&src->kev, dst);
#if defined(HAVE_EPOLLRDHUP)
    if (ev->events & EPOLLRDHUP || ev->events & EPOLLHUP)
        dst->flags |= EV_EOF;
#else
    if (ev->events & EPOLLHUP)
        dst->flags |= EV_EOF;
#endif
    if (ev->events & EPOLLERR)
        dst->fflags = 1; /* FIXME: Return the actual socket error */
          
    /* On return, data contains the space remaining in the write buffer. */
    dst->data = write_space_avail(dst->ident);

    return (0);
}

int
evfilt_socket_knote_create(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;

    if (linux_get_descriptor_type(kn) < 0)
        return (-1);

#ifndef DARLING
    /* TODO: return EBADF? */
    if (kn->kn_flags & KNFL_REGULAR_FILE)
        return (-1);
#endif

    /* Convert the kevent into an epoll_event */
    kn->data.events = EPOLLOUT;
    if (kn->kev.flags & EV_ONESHOT || kn->kev.flags & EV_DISPATCH)
        kn->data.events |= EPOLLONESHOT;
    if (kn->kev.flags & EV_CLEAR)
        kn->data.events |= EPOLLET;
	kn->kn_epollfd = filter_epfd(filt);

    memset(&ev, 0, sizeof(ev));
    ev.events = kn->data.events;
    ev.data.ptr = kn;

#ifdef DARLING
    /* A regular file is always writable to XNU (EVFILT_WRITE on a vnode), but epoll refuses regular files
     * (EPERM): the registration failed (kevent EBADF), so a dispatch write source on a file never fired.
     * A receiver writing incoming data into a temporary file through such a source stalled at 0 bytes and
     * the resource was never delivered. An eventfd stands in for the file: always writable (EPOLLOUT), as
     * read.c's surrogate is always readable; the copyout reports 1 byte of space (no socket buffer to measure). */
    if (kn->kn_flags & KNFL_REGULAR_FILE)
        kn->kdata.kn_dupfd = eventfd(0, EFD_CLOEXEC);
    else
#endif
    /* Duplicate the fd to workaround epoll's poor design */
	kn->kdata.kn_dupfd = _dup_4libkqueue(kn->kev.ident);

	if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_ADD, kn->kdata.kn_dupfd, &ev) < 0) {
		dbg_printf("epoll_ctl(2): %s", strerror(errno));
		return (-1);
	}
    return 0;
}

int
evfilt_socket_knote_modify(struct filter *filt, struct knote *kn, 
        const struct kevent64_s *kev)
{
    struct epoll_event ev;

    /* Convert the kevent into an epoll_event */
    kn->data.events = EPOLLOUT;
    if (kn->kev.flags & EV_ONESHOT || kn->kev.flags & EV_DISPATCH)
        kn->data.events |= EPOLLONESHOT;
    if (kn->kev.flags & EV_CLEAR)
        kn->data.events |= EPOLLET;
	kn->kn_epollfd = filter_epfd(filt);

    /* Disabled registrations are added by the enable callback, not MOD. */
    if (kn->kev.flags & EV_DISABLE)
        return (0);

    memset(&ev, 0, sizeof(ev));
    ev.events = kn->data.events;
    ev.data.ptr = kn;

	if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_MOD, kn->kdata.kn_dupfd, &ev) < 0) {
		dbg_printf("epoll_ctl(2): %s", strerror(errno));
		return (-1);
	}
    return 0;
}

int
evfilt_socket_knote_delete(struct filter *filt, struct knote *kn)
{
    if (kn->kev.flags & EV_DISABLE)
    {
        (void) __close_for_kqueue(kn->kdata.kn_dupfd);
        kn->kdata.kn_dupfd = -1;
        return (0);
    }
    else {
        if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_DEL, kn->kdata.kn_dupfd, NULL) < 0) {
            dbg_perror("epoll_ctl(2)");
            return (-1);
        }
        (void) __close_for_kqueue(kn->kdata.kn_dupfd);
        kn->kdata.kn_dupfd = -1;
		return 0;
	}
}

int
evfilt_socket_knote_enable(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.events = kn->data.events;
    ev.data.ptr = kn;

	if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_ADD, kn->kdata.kn_dupfd, &ev) < 0) {
		dbg_perror("epoll_ctl(2)");
		return (-1);
	}
	return (0);
}

int
evfilt_socket_knote_disable(struct filter *filt, struct knote *kn)
{
	if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_DEL, kn->kdata.kn_dupfd, NULL) < 0) {
		dbg_perror("epoll_ctl(2)");
		return (-1);
	}
	return (0);
}

const struct filter evfilt_write = {
    EVFILT_WRITE,
    NULL,
    NULL,
    evfilt_socket_copyout,
    evfilt_socket_knote_create,
    evfilt_socket_knote_modify,
    evfilt_socket_knote_delete,
    evfilt_socket_knote_enable,
    evfilt_socket_knote_disable,         
};
