/*
 * Copyright (c) 2017 Lubos Dolezel
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
#include <sys/socket.h>
#include <sys/types.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <mach/message.h>
#include <darling/emulation/linux_premigration/ext/for-libkqueue.h>
#include <darlingserver/rpc-supplement.h>

#include "private.h"

/*
 * EVFILT_MACHPORT knotes are backed by a kqueue channel (kqchan) to darlingserver,
 * which owns the port. The server tells us about events on the channel's socket.
 *
 * Once a knote's channel is armed (dserver_kqchan_msgnum_mach_port_arm), the server
 * processes the knote itself (receiving the message, if the knote asks for MACH_RCV_MSG)
 * when the port has a message while one of our threads waits in kevent for the knote,
 * and sends us the resulting event. Otherwise it sends a notification, and we read the
 * event when we copy it out (dserver_kqchan_msgnum_mach_port_read), as before.
 *
 * A thread waiting in kevent marks the enabled knotes of its kqueue as armed in memory the
 * server reads (kn_mp.armed). When the last waiter stops waiting, it clears the marks and then
 * collects the events the server started processing before it saw the cleared mark
 * (kn_mp.sequence is ahead of what we have received), so that an event the server processed
 * during a kevent call is returned by that call, as kevent processes knotes on macOS.
 *
 * The value of kn_mp.armed changes with every wait, and the server processes the knote inline at most
 * once per value: the message of that event is in the knote's receive buffer until the caller of kevent
 * is done with it. For a later event, the server sets kn_mp.wanted instead, and the next kevent call
 * reads the event (as it does after an unprocessed notification).
 *
 * After an event, if the port still has messages (DSERVER_KQCHAN_MACH_PORT_MORE), the server
 * sends nothing more for the knote: the knote is level-triggered, and the next kevent call that
 * waits for it must look at the port again. That call reads the next event itself.
 */

#define MP_ARMED_ONCE       0x01  /* the channel was armed; the server may access kn_mp */
#define MP_MORE             0x02  /* the port had more messages after the last event; read the next one */
#define MP_PENDING_READ     0x04  /* the server sent an unprocessed notification */
#define MP_LISTED           0x08  /* the knote is in kq_machports */

struct linux_machport_event {
    struct linux_machport_event *next;
    uint16_t flags;
    uint32_t fflags;
    int64_t data;
    uint64_t ext[2];
    int consumed; /* the event took a message off the port */
};

union mp_message {
    dserver_kqchan_replyhdr_t reply;
    dserver_kqchan_call_notification_t notification;
    dserver_kqchan_mach_port_event_t event;
    dserver_kqchan_reply_mach_port_read_t read;
    dserver_kqchan_reply_mach_port_modify_t modify;
    dserver_kqchan_reply_mach_port_detach_t detach;
};

static int
mp_sequence_newer(uint32_t a, uint32_t b)
{
    return (int32_t) (a - b) > 0;
}

static int
mp_enabled(const struct knote *kn)
{
    return !(kn->kn_flags & KNFL_KNOTE_DELETED) && !(kn->kev.flags & EV_DISABLE);
}

/*
 * Whether an event took the message off the port: the knote asked for MACH_RCV_MSG and the
 * message was not left queued for being too large (MACH_RCV_LARGE, which is implied when the
 * knote has no buffer of its own).
 */
static int
mp_event_consumed(const struct knote *kn, uint32_t fflags)
{
    if (!(kn->kev.fflags & MACH_RCV_MSG))
        return 0;
    if (fflags == MACH_RCV_TOO_LARGE && ((kn->kev.fflags & MACH_RCV_LARGE) || kn->kev.ext[1] == 0))
        return 0;
    return 1;
}

static void
mp_push_event(struct knote *kn, uint16_t flags, uint32_t fflags, int64_t data, const uint64_t ext[2])
{
    struct linux_machport_event *ev = malloc(sizeof(*ev));

    if (ev == NULL) {
        /* there is no way to report this without losing the event */
        abort();
    }

    ev->next = NULL;
    ev->flags = flags;
    ev->fflags = fflags;
    ev->data = data;
    ev->ext[0] = ext[0];
    ev->ext[1] = ext[1];
    ev->consumed = mp_event_consumed(kn, fflags);

    *kn->kn_mp.pending_tail = ev;
    kn->kn_mp.pending_tail = &ev->next;
}

static struct linux_machport_event *
mp_pop_event(struct knote *kn)
{
    struct linux_machport_event *ev = kn->kn_mp.pending;

    if (ev != NULL) {
        kn->kn_mp.pending = ev->next;
        if (kn->kn_mp.pending == NULL)
            kn->kn_mp.pending_tail = &kn->kn_mp.pending;
    }
    return ev;
}

/* Drops the pending events that did not take a message; returns whether any are left. */
static int
mp_drop_unconsumed_events(struct knote *kn)
{
    struct linux_machport_event **link = &kn->kn_mp.pending, *ev;

    while ((ev = *link) != NULL) {
        if (!ev->consumed) {
            *link = ev->next;
            free(ev);
        } else {
            link = &ev->next;
        }
    }
    kn->kn_mp.pending_tail = link;
    return kn->kn_mp.pending != NULL;
}

static void
mp_apply(struct kevent64_s *dst, uint16_t flags, uint32_t fflags, int64_t data, const uint64_t ext[2])
{
    if (flags != 0)
        dst->flags = flags;
    dst->data = data;
    dst->ext[0] = ext[0];
    dst->ext[1] = ext[1];
    dst->fflags = fflags;
}

/* Makes the waiting threads look at the knotes that have events outside of their sockets. */
static void
mp_wake_raise(struct kqueue *kq)
{
    if (kq->kq_mp_wakefd >= 0 && !kq->kq_mp_wake_raised) {
        uint64_t one = 1;
        if (write(kq->kq_mp_wakefd, &one, sizeof(one)) == sizeof(one))
            kq->kq_mp_wake_raised = 1;
    }
}

/* Handles a message the server sent on its own; returns -1 for any other message. */
static int
mp_handle_server_message(struct knote *kn, const union mp_message *msg, ssize_t len)
{
    if (len < (ssize_t) sizeof(msg->reply))
        return (-1);

    switch (msg->reply.number) {
        case dserver_kqchan_msgnum_notification:
            kn->kn_mp.flags |= MP_PENDING_READ;
            return (0);

        case dserver_kqchan_msgnum_mach_port_event:
            if (len < (ssize_t) sizeof(msg->event))
                return (-1);
            if (mp_sequence_newer(msg->event.sequence, kn->kn_mp.consumed))
                kn->kn_mp.consumed = msg->event.sequence;
            switch (msg->event.kind) {
                case dserver_kqchan_mach_port_event_unprocessed:
                    kn->kn_mp.flags |= MP_PENDING_READ;
                    break;
                case dserver_kqchan_mach_port_event_processed: {
                    uint64_t ext[2] = { msg->event.kev.ext[0], msg->event.kev.ext[1] };
                    mp_push_event(kn, msg->event.kev.flags, msg->event.kev.fflags, msg->event.kev.data, ext);
                    if (msg->event.flags & DSERVER_KQCHAN_MACH_PORT_MORE)
                        kn->kn_mp.flags |= MP_MORE;
                } break;
                default:
                    /* the server found no event after all */
                    break;
            }
            return (0);

        default:
            return (-1);
    }
}

/*
 * Receives messages until the reply numbered `number` arrives (into `reply`), handling the
 * messages the server sends on its own in between.
 */
static int
mp_wait_reply(struct knote *kn, dserver_kqchan_msgnum_t number, void *reply, size_t reply_size)
{
    union mp_message msg;
    ssize_t rv;

    for (;;) {
        rv = recv(kn->kdata.kn_dupfd, &msg, sizeof(msg), 0);
        if (rv < 0) {
            if (errno == EINTR)
                continue;
            dbg_printf("machport: recv failed: %d (%s)", errno, strerror(errno));
            return (-1);
        }
        if (rv == 0) {
            dbg_puts("machport: the server closed the channel");
            return (-1);
        }
        if (rv >= (ssize_t) sizeof(msg.reply) && msg.reply.number == number) {
            if (rv < (ssize_t) reply_size) {
                dbg_puts("machport: reply too short");
                return (-1);
            }
            memcpy(reply, &msg, reply_size);
            return (0);
        }
        if (mp_handle_server_message(kn, &msg, rv) < 0) {
            dbg_printf("machport: unexpected message %d", msg.reply.number);
            return (-1);
        }
    }
}

/* Receives the events the server has started to send (see the comment at the top). */
static void
mp_collect(struct knote *kn)
{
    uint32_t target = __atomic_load_n(&kn->kn_mp.sequence, __ATOMIC_SEQ_CST);
    union mp_message msg;
    ssize_t rv;

    while (mp_sequence_newer(target, kn->kn_mp.consumed)) {
        rv = recv(kn->kdata.kn_dupfd, &msg, sizeof(msg), 0);
        if (rv < 0 && errno == EINTR)
            continue;
        if (rv <= 0 || mp_handle_server_message(kn, &msg, rv) < 0) {
            dbg_printf("machport: failed to collect an event (%d, %d)", (int) rv, errno);
            break;
        }
    }
}

static int
mp_send_arm(struct knote *kn)
{
    dserver_kqchan_call_mach_port_arm_t call;

    call.header.number = dserver_kqchan_msgnum_mach_port_arm;
    call.header.pid = getpid();
    call.header.tid = THREAD_ID;
    call.shared_state = (uint64_t) (uintptr_t) &kn->kn_mp.armed;
    call.default_buffer = (uint64_t) (uintptr_t) &kn->kn_extra_buffer[0];
    call.default_buffer_size = sizeof(kn->kn_extra_buffer);

    if (send(kn->kdata.kn_dupfd, &call, sizeof(call), 0) < 0) {
        dbg_printf("machport: sending arm failed: %d (%s)", errno, strerror(errno));
        return (-1);
    }
    return (0);
}

/*
 * Marks the knote armed if a thread waits on its kqueue. If the port still had messages after the
 * last event, the knote is still active: read its next event in this kevent call.
 */
/* Turns the server's request to read the knote's next event (see the comment at the top) into a pending read. */
static void
mp_check_wanted(struct knote *kn)
{
    if (__atomic_load_n(&kn->kn_mp.wanted, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&kn->kn_mp.wanted, 0, __ATOMIC_SEQ_CST);
        kn->kn_mp.flags |= MP_PENDING_READ;
    }
}

static __thread struct linux_machport_waiter mp_self;

/*
 * The server receives the knote's messages in the context of the waiting thread that the knote names:
 * that thread makes no other server calls until it has stopped waiting, changed the knote's `armed`
 * value, and collected the events that the server started to process for it.
 */
static void
mp_arm(struct knote *kn)
{
    struct linux_machport_waiter *waiter = LIST_FIRST(&kn->kn_kq->kq_mp_waiters);

    if (!(kn->kn_mp.flags & MP_ARMED_ONCE) || !mp_enabled(kn) || waiter == NULL)
        return;

    if (++kn->kn_mp.arming == 0)
        kn->kn_mp.arming = 1;
    __atomic_store_n(&kn->kn_mp.thread, waiter->tid, __ATOMIC_SEQ_CST);
    __atomic_store_n(&kn->kn_mp.armed, kn->kn_mp.arming, __ATOMIC_SEQ_CST);
    mp_check_wanted(kn);

    if (kn->kn_mp.flags & MP_MORE)
        kn->kn_mp.flags = (kn->kn_mp.flags & ~MP_MORE) | MP_PENDING_READ;
}

static void
mp_disarm(struct knote *kn)
{
    __atomic_store_n(&kn->kn_mp.armed, 0, __ATOMIC_SEQ_CST);
}

static int
mp_has_work(const struct knote *kn)
{
    return kn->kn_mp.pending != NULL || (kn->kn_mp.flags & MP_PENDING_READ);
}

/* Reads the event with a round trip; used when the server did not process the knote. */
static int
mp_read(struct kevent64_s *dst, struct knote *kn)
{
    dserver_kqchan_call_mach_port_read_t call;
    dserver_kqchan_reply_mach_port_read_t reply;

    call.header.number = dserver_kqchan_msgnum_mach_port_read;
    call.header.pid = getpid();
    call.header.tid = THREAD_ID;
    call.default_buffer = (uint64_t) (uintptr_t) &kn->kn_extra_buffer[0];
    call.default_buffer_size = sizeof(kn->kn_extra_buffer);
    if (send(kn->kdata.kn_dupfd, &call, sizeof(call), 0) < 0) {
        dbg_printf("evfilt_machport_copyout() sending request failed: %d (%s)", errno, strerror(errno));
        return (-1);
    }

    if (mp_wait_reply(kn, dserver_kqchan_msgnum_mach_port_read, &reply, sizeof(reply)) < 0) {
        dbg_puts("evfilt_machport_copyout() reading reply failed");
        return (-1);
    }

    if (reply.header.code == 0xdead) {
        // server indicated there was actually no event available to read right now;
        // drop the event
        dst->filter = EVFILT_DROP;
        return (0);
    }

    if (reply.header.code == DSERVER_KQCHAN_MACH_PORT_MORE) {
        kn->kn_mp.flags |= MP_MORE;
    } else if (reply.header.code != 0) {
        // FIXME: the returned code is actually a Linux code (but strerror is provided by Darwin libc here)
        dbg_printf("evfilt_machport_copyout() server indicated failure: %d (%s)", -reply.header.code, strerror(-reply.header.code));
        return (-1);
    }

    {
        uint64_t ext[2] = { reply.kev.ext[0], reply.kev.ext[1] };
        mp_apply(dst, reply.kev.flags, reply.kev.fflags, reply.kev.data, ext);
    }

    // TODO: we need to properly support kevent_qos with regards to the data_out argument;
    //       this is what's supposed to be passed in as the default buffer, not a buffer of our own.

    return (0);
}

int
evfilt_machport_copyout(struct kevent64_s *dst, struct knote *kn, void *ptr)
{
    struct epoll_event * const ev = (struct epoll_event *) ptr;
    struct linux_machport_event *pending;

    epoll_event_dump(ev);
    kevent_int_to_64(&kn->kev, dst);

    if (!mp_has_work(kn) && kn->kdata.kn_dupfd >= 0) {
        union mp_message msg;
        ssize_t rv;

        /* another thread may have taken the event already */
        rv = recv(kn->kdata.kn_dupfd, &msg, sizeof(msg), MSG_DONTWAIT);
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            dst->filter = EVFILT_DROP;
            return (0);
        }
        if (rv <= 0) {
            dbg_printf("evfilt_machport_copyout() reading notification failed: %d, %d (%s)", (int) rv, errno, strerror(errno));
            return (-1);
        }
        if (mp_handle_server_message(kn, &msg, rv) < 0) {
            dbg_puts("evfilt_machport_copyout() read invalid notification");
            return (-1);
        }
    }

    if ((pending = mp_pop_event(kn)) != NULL) {
        mp_apply(dst, pending->flags, pending->fflags, pending->data, pending->ext);
        free(pending);
        return (0);
    }

    if ((kn->kn_mp.flags & MP_PENDING_READ) && !(kn->kn_flags & KNFL_KNOTE_DELETED)) {
        kn->kn_mp.flags &= ~MP_PENDING_READ;
        return (mp_read(dst, kn));
    }

    dst->filter = EVFILT_DROP;
    return (0);
}

/* The eventfd that wakes waiters for events kept outside of the channel sockets (see mp_wake_raise()). */
static int
mp_wake_init(struct kqueue *kq)
{
    struct epoll_event ev;
    int fd;

    if (kq->kq_mp_wakefd >= 0)
        return (0);

    fd = eventfd(0, 0);
    if (fd < 0) {
        dbg_perror("eventfd");
        return (-1);
    }
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        dbg_perror("fcntl");
        (void) __close_for_kqueue(fd);
        return (-1);
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &kq->kq_mp_wakefd;
    if (epoll_ctl(kq->kq_id, EPOLL_CTL_ADD, fd, &ev) < 0) {
        dbg_perror("epoll_ctl(2)");
        (void) __close_for_kqueue(fd);
        return (-1);
    }

    kq->kq_mp_wakefd = fd;
    kq->kq_mp_wake_raised = 0;
    return (0);
}

int
evfilt_machport_knote_create(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;
    struct kqueue *kq = kn->kn_kq;
    int port = kn->kev.ident;

    /* Convert the kevent into an epoll_event */
    kn->data.events = EPOLLIN;
    kn->kn_epollfd = filter_epfd(filt);

    memset(&ev, 0, sizeof(ev));
    ev.events = kn->data.events;
    ev.data.ptr = kn;

    memset(&kn->kn_mp, 0, sizeof(kn->kn_mp));
    kn->kn_mp.pending_tail = &kn->kn_mp.pending;

	int status = _dserver_rpc_kqchan_mach_port_open_4libkqueue(port, (void*)kn->kev.ext[0], kn->kev.ext[1], kn->kev.fflags, &kn->kdata.kn_dupfd);
	if (status < 0) {
		dbg_printf("evfilt_machport_open: %s", strerror(-status));
		return (-1);
	}

	dbg_printf("evfilt_machport_open: listening to FD %d for events %d", kn->kdata.kn_dupfd, ev.events);

    fcntl(kn->kdata.kn_dupfd, F_SETFD, FD_CLOEXEC);

    if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_ADD, kn->kdata.kn_dupfd, &ev) < 0) {
        dbg_printf("epoll_ctl(2): %s", strerror(errno));
        (void) __close_for_kqueue(kn->kdata.kn_dupfd);
        kn->kdata.kn_dupfd = -1;
        return (-1);
    }

    LIST_INSERT_HEAD(&kq->kq_machports, kn, kn_mp.entries);
    kn->kn_mp.flags |= MP_LISTED;

    /* without the wake descriptor, events could wait in a knote for an unrelated wakeup: read them instead */
    if (mp_wake_init(kq) == 0 && mp_send_arm(kn) == 0) {
        kn->kn_mp.flags |= MP_ARMED_ONCE;
        mp_arm(kn);
    }

    return 0;
}

int
evfilt_machport_knote_modify(struct filter *filt, struct knote *kn,
        const struct kevent64_s *kev)
{
	dserver_kqchan_call_mach_port_modify_t call;
	dserver_kqchan_reply_mach_port_modify_t reply = {0};
	int rv;

	call.header.number = dserver_kqchan_msgnum_mach_port_modify;
	call.header.pid = getpid();
	call.header.tid = THREAD_ID;
	call.receive_buffer = kev->ext[0];
	call.receive_buffer_size = kev->ext[1];
	call.saved_filter_flags = kev->fflags;

	rv = send(kn->kdata.kn_dupfd, &call, sizeof(call), 0);
	if (rv < 0) {
		dbg_printf("evfilt_machport_knote_modify send failed: %d (%s)", errno, strerror(errno));
		return -1;
	}

	if (mp_wait_reply(kn, dserver_kqchan_msgnum_mach_port_modify, &reply, sizeof(reply)) < 0) {
		dbg_puts("evfilt_machport_knote_modify invalid reply");
		return -1;
	}

	if (reply.header.code != 0) {
		// FIXME: same as in copyout: Linux code but Darwin strerror
		dbg_printf("evfilt_machport_knote_modify call failed: %d (%s)", -reply.header.code, strerror(-reply.header.code));
		return -1;
	}

	/* what the events we deliver from now on are interpreted against */
	kn->kev.fflags = kev->fflags;
	kn->kev.ext[0] = kev->ext[0];
	kn->kev.ext[1] = kev->ext[1];

	/* events received while waiting for the reply are no longer in the socket */
	if (mp_has_work(kn) && mp_enabled(kn))
		mp_wake_raise(kn->kn_kq);

    return 0;
}

static void
mp_unlist(struct knote *kn)
{
    struct linux_machport_event *ev;

    while ((ev = mp_pop_event(kn)) != NULL)
        free(ev);
    if (kn->kn_mp.flags & MP_LISTED) {
        LIST_REMOVE(kn, kn_mp.entries);
        kn->kn_mp.flags &= ~MP_LISTED;
    }
}

int
evfilt_machport_knote_delete(struct filter *filt, struct knote *kn)
{
    /* the knote of a forked child shares the channel with the parent's knote: leave the channel alone */
    int forked = (kn->kn_kq->kq_id < 0);

    if ((kn->kev.flags & EV_DISABLE) == 0 && !forked) {
        /* The channel is closed even if its epoll entry is already
         * gone; the knote is freed regardless of the result. */
        if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_DEL, kn->kdata.kn_dupfd, NULL) < 0)
            dbg_perror("epoll_ctl(2)");
    }

    if ((kn->kn_mp.flags & MP_ARMED_ONCE) && !forked) {
        dserver_kqchan_call_mach_port_detach_t call;
        dserver_kqchan_reply_mach_port_detach_t reply;

        mp_disarm(kn);

        /*
         * The server may write the knote's memory (or receive a message for it) until it has
         * acknowledged the detach; the events it sends before that are collected on the way.
         */
        call.header.number = dserver_kqchan_msgnum_mach_port_detach;
        call.header.pid = getpid();
        call.header.tid = THREAD_ID;
        if (send(kn->kdata.kn_dupfd, &call, sizeof(call), 0) < 0
                || mp_wait_reply(kn, dserver_kqchan_msgnum_mach_port_detach, &reply, sizeof(reply)) < 0)
            dbg_puts("machport: detaching the channel failed");
    }

	(void) __close_for_kqueue(kn->kdata.kn_dupfd);
	kn->kdata.kn_dupfd = -1;
	kn->kn_mp.flags &= ~MP_PENDING_READ;

    /*
     * A message received for the knote before it was deleted was taken off the port
     * while a thread waited for the knote: that thread's kevent call (or the next one) returns it.
     */
    if (!forked && (kn->kn_mp.flags & MP_LISTED) && mp_drop_unconsumed_events(kn)) {
        knote_retain(kn);
        mp_wake_raise(kn->kn_kq);
    } else {
        mp_unlist(kn);
    }
	return 0;
}

int
evfilt_machport_knote_enable(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.events = kn->data.events;
    ev.data.ptr = kn;

	dbg_printf("enabling machport knote with ID=%llu for events %d", kn->kev.ident, ev.events);

	if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_ADD, kn->kdata.kn_dupfd, &ev) < 0) {
		dbg_perror("epoll_ctl(2)");
		return (-1);
	}

    mp_arm(kn);
    if (mp_has_work(kn))
        mp_wake_raise(kn->kn_kq);
	return (0);
}

int
evfilt_machport_knote_disable(struct filter *filt, struct knote *kn)
{
	dbg_printf("disable machport knote with ID=%llu", kn->kev.ident);

    /* an event that the server is processing now is collected by the thread it names, and kept until the knote is enabled again */
    mp_disarm(kn);

	if (epoll_ctl(kn->kn_epollfd, EPOLL_CTL_DEL, kn->kdata.kn_dupfd, NULL) < 0) {
		dbg_perror("epoll_ctl(2)");
		return (-1);
	}
	return (0);
}

static void
evfilt_machport_destroy(struct filter *filt)
{
    struct kqueue *kq = filt->kf_kqueue;
    struct knote *kn;

    /* only deleted knotes whose events were never delivered are left */
    while ((kn = LIST_FIRST(&kq->kq_machports)) != NULL) {
        mp_unlist(kn);
        if (kn->kn_flags & KNFL_KNOTE_DELETED)
            knote_release(kn);
    }
}

int
linux_machport_wait_enter(struct kqueue *kq)
{
    struct knote *kn;
    int pending = 0;

    if (mp_self.tid == 0)
        mp_self.tid = THREAD_ID;
    LIST_INSERT_HEAD(&kq->kq_mp_waiters, &mp_self, entries);

    LIST_FOREACH(kn, &kq->kq_machports, kn_mp.entries) {
        if (linux_machport_deliverable(kn)) {
            pending = 1;
            continue;
        }
        if (!mp_enabled(kn))
            continue;
        mp_arm(kn);
        if (mp_has_work(kn))
            pending = 1;
    }
    return (pending);
}

int
linux_machport_is_wake_event(struct kqueue *kq, struct epoll_event *ev)
{
    return ev->data.ptr == (void *) &kq->kq_mp_wakefd;
}

int
linux_machport_wait_exit(struct kqueue *kq, int nready, int nevents)
{
    struct epoll_event *epevt;
    struct knote *kn;
    int i, j, n;

    LIST_REMOVE(&mp_self, entries);

    if (LIST_EMPTY(&kq->kq_machports) && kq->kq_mp_wakefd < 0)
        return (nready);

    /*
     * The event this thread returns may have its message in the knote's receive buffer. If other
     * threads still wait, the knote is armed anew for one of them (and a message received for them
     * may replace that one, as it would if the knote were processed on their behalf on macOS).
     */
    LIST_FOREACH(kn, &kq->kq_machports, kn_mp.entries) {
        if (LIST_EMPTY(&kq->kq_mp_waiters))
            mp_disarm(kn);
        else
            mp_arm(kn);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* disabled knotes, too: the server may be receiving a message for one in this thread's context */
    LIST_FOREACH(kn, &kq->kq_machports, kn_mp.entries) {
        if ((kn->kn_mp.flags & MP_ARMED_ONCE) && kn->kdata.kn_dupfd >= 0) {
            mp_collect(kn);
            mp_check_wanted(kn);
        }
    }

    if (nready < 0)
        return (nready);

    epevt = linux_kevent_epevt();

    /* drop the wakeup itself */
    for (i = 0, n = 0; i < nready; i++) {
        if (linux_machport_is_wake_event(kq, &epevt[i])) {
            uint64_t count;
            if (read(kq->kq_mp_wakefd, &count, sizeof(count)) < 0)
                dbg_perror("read(2) of the wake descriptor");
            kq->kq_mp_wake_raised = 0;
            continue;
        }
        epevt[n++] = epevt[i];
    }
    nready = n;

    /* add the knotes that have events outside of their sockets */
    LIST_FOREACH(kn, &kq->kq_machports, kn_mp.entries) {
        if (!linux_machport_deliverable(kn) && !(mp_enabled(kn) && mp_has_work(kn)))
            continue;
        for (j = 0; j < nready; j++) {
            if (epevt[j].data.ptr == kn)
                break;
        }
        if (j < nready)
            continue;
        if (nready >= nevents) {
            /* no room in this call; the next one returns it */
            mp_wake_raise(kq);
            break;
        }
        memset(&epevt[nready], 0, sizeof(epevt[nready]));
        epevt[nready].events = EPOLLIN;
        epevt[nready].data.ptr = kn;
        nready++;
    }

    return (nready);
}

int
linux_machport_deliverable(struct knote *kn)
{
    return kn->kev.filter == EVFILT_MACHPORT && (kn->kn_flags & KNFL_KNOTE_DELETED)
        && (kn->kn_mp.flags & MP_LISTED) && kn->kn_mp.pending != NULL;
}

void
linux_machport_delivered(struct knote *kn)
{
    if (kn->kev.filter != EVFILT_MACHPORT || !(kn->kn_mp.flags & MP_LISTED))
        return;
    if (kn->kn_mp.pending != NULL) {
        mp_wake_raise(kn->kn_kq);
        return;
    }
    mp_unlist(kn);
    knote_release(kn);
}

void
linux_machport_after_copyout(struct kqueue *kq)
{
    struct knote *kn;
    int wake = 0;

    /* if other threads still wait, they look at the knotes that are still active */
    if (LIST_EMPTY(&kq->kq_mp_waiters))
        return;
    LIST_FOREACH(kn, &kq->kq_machports, kn_mp.entries) {
        if (kn->kn_mp.flags & MP_MORE) {
            mp_arm(kn);
            wake |= mp_has_work(kn);
        }
    }
    if (wake)
        mp_wake_raise(kq);
}

const struct filter evfilt_machport = {
    EVFILT_MACHPORT,
    NULL,
    evfilt_machport_destroy,
    evfilt_machport_copyout,
    evfilt_machport_knote_create,
    evfilt_machport_knote_modify,
    evfilt_machport_knote_delete,
    evfilt_machport_knote_enable,
    evfilt_machport_knote_disable,
};
