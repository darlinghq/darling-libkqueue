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

#ifndef NDEBUG
static char *
inotify_mask_dump(uint32_t mask)
{
    static __thread char buf[1024];

#define INEVT_MASK_DUMP(attrib) \
    if (mask & attrib) \
       strcat(buf, #attrib" ");

    snprintf(buf, sizeof(buf), "mask = %d (", mask);
    INEVT_MASK_DUMP(IN_ACCESS);
    INEVT_MASK_DUMP(IN_MODIFY);
    INEVT_MASK_DUMP(IN_ATTRIB);
    INEVT_MASK_DUMP(IN_CLOSE_WRITE);
    INEVT_MASK_DUMP(IN_CLOSE_NOWRITE);
    INEVT_MASK_DUMP(IN_OPEN);
    INEVT_MASK_DUMP(IN_MOVED_FROM);
    INEVT_MASK_DUMP(IN_MOVED_TO);
    INEVT_MASK_DUMP(IN_CREATE);
    INEVT_MASK_DUMP(IN_DELETE);
    INEVT_MASK_DUMP(IN_DELETE_SELF);
    INEVT_MASK_DUMP(IN_MOVE_SELF);
    buf[strlen(buf) - 1] = ')';

    return (buf);
}

static char *
inotify_event_dump(struct inotify_event *evt)
{
    static __thread char buf[1024];

    snprintf(buf, sizeof(buf), "wd=%d mask=%s", 
            evt->wd,
            inotify_mask_dump(evt->mask));

    return (buf);
}

#endif /* !NDEBUG */


#ifdef __APPLE__
# define VNODE_MTIME(sb) ((sb)->st_mtimespec)
#else
# define VNODE_MTIME(sb) ((sb)->st_mtim)
#endif

/* Remember the state of the vnode that the next events are compared to */
static void
vnode_snapshot(struct knote *kn, const struct stat *sb)
{
    kn->data.vnode.nlink = sb->st_nlink;
    kn->data.vnode.size = sb->st_size;
    kn->data.vnode.mode = sb->st_mode;
    kn->data.vnode.uid = sb->st_uid;
    kn->data.vnode.gid = sb->st_gid;
    kn->data.vnode.mtime_sec = VNODE_MTIME(sb).tv_sec;
    kn->data.vnode.mtime_nsec = VNODE_MTIME(sb).tv_nsec;
}

/* Whether an attribute other than the link count and size changed */
static int
vnode_attrs_changed(const struct knote *kn, const struct stat *sb)
{
    return (sb->st_mode != kn->data.vnode.mode
            || sb->st_uid != kn->data.vnode.uid
            || sb->st_gid != kn->data.vnode.gid
            || VNODE_MTIME(sb).tv_sec != kn->data.vnode.mtime_sec
            || VNODE_MTIME(sb).tv_nsec != kn->data.vnode.mtime_nsec);
}

/* Changes to the entries of a watched directory */
#define IN_ENTRY_EVENTS (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)

/* Read every event queued on the (nonblocking) inotify descriptor into buf.
   Returns the byte count, 0 if nothing is queued, -1 on error. */
static ssize_t
read_events(char *buf, size_t len, int inofd)
{
    ssize_t n;

    for (;;) {
        n = read(inofd, buf, len);
        if (n >= 0)
            break;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN)
            return (0);
        dbg_perror("read");
        return (-1);
    }
    dbg_printf("read(2) from inotify wd: %ld bytes", (long) n);

    return (n);
}

static int
add_watch(struct filter *filt, struct knote *kn)
{
    struct epoll_event ev;
    int ifd;
    char path[PATH_MAX];
    uint32_t mask;

    /* Convert the fd to a pathname */
    if (linux_fd_to_path(&path[0], sizeof(path), kn->kev.ident) < 0)
        return (-1);

    /* Convert the fflags to the inotify mask */
    /* Closing another descriptor for this inode is not a vnode event, so
       IN_CLOSE is not watched. */
    mask = 0;
    if (kn->kev.fflags & NOTE_DELETE)
        mask |= IN_ATTRIB | IN_DELETE_SELF;
    /* Adding, removing or renaming an entry of a directory writes to the
       directory; inotify reports those only for directories. */
    if (kn->kev.fflags & NOTE_WRITE)
        mask |= IN_MODIFY | IN_ENTRY_EVENTS;
    if (kn->kev.fflags & NOTE_EXTEND)
        mask |= IN_MODIFY;
    if (kn->kev.fflags & NOTE_ATTRIB)
        mask |= IN_ATTRIB;
    /* A subdirectory's ".." is a link to its parent */
    if (kn->kev.fflags & NOTE_LINK)
        mask |= IN_ATTRIB | IN_ENTRY_EVENTS;
    if (kn->kev.fflags & NOTE_RENAME)
        mask |= IN_MOVE_SELF;
    /* No IN_ONESHOT: it would consume the watch on an event that copyout
       drops, while the core only deletes a one-shot knote on delivery. */
    if (mask == 0)
        mask = IN_ATTRIB; /* inotify rejects an empty mask; its events are dropped */

    /* Create an inotify descriptor */
    ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) {
        dbg_perror("inotify_init1(2)");
        return (-1);
    }

    /* Add the watch */
    dbg_printf("inotify_add_watch(2); inofd=%d, %s, path=%s", 
            ifd, inotify_mask_dump(mask), path);
    kn->kev.data = inotify_add_watch(ifd, path, mask);
    if (kn->kev.data < 0) {
        dbg_perror("inotify_add_watch(2)");
        goto errout;
    }

    /* Add the inotify fd to the epoll set */
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = kn;
    if (epoll_ctl(filter_epfd(filt), EPOLL_CTL_ADD, ifd, &ev) < 0) {
        dbg_perror("epoll_ctl(2)");
        goto errout;
    }

    kn->kdata.kn_inotifyfd = ifd;

    return (0);

errout:
    kn->kdata.kn_inotifyfd = -1;
    (void) __close_for_kqueue(ifd);
    return (-1);
}

static int
delete_watch(struct filter *filt, struct knote *kn)
{
    int ifd = kn->kdata.kn_inotifyfd;

    if (ifd < 0)
        return (0);
    if (epoll_ctl(filter_epfd(filt), EPOLL_CTL_DEL, ifd, NULL) < 0) {
        dbg_perror("epoll_ctl(2)");
        return (-1);
    }
    (void) __close_for_kqueue(ifd);
    kn->kdata.kn_inotifyfd = -1;

    return (0);
}

int
evfilt_vnode_copyout(struct kevent64_s *dst, struct knote *src, void *ptr UNUSED)
{
    /* One read returns every queued event that fits; all of them belong to
       this copyout, so none may be left to be dropped with the first. */
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    struct inotify_event *evt;
    struct stat sb;
    uint32_t mask = 0;      /* events of the vnode itself */
    uint32_t fflags = 0;
    int entries = 0;        /* an entry of the directory changed */
    int subdirs = 0;        /* a subdirectory was added or removed */
    int moved_dir = 0;      /* a subdirectory was renamed from this directory... */
    uint32_t moved_cookie = 0; /* ...in the rename with this cookie */
    int attrib;
    ssize_t n, off;

    n = read_events(buf, sizeof(buf), src->kdata.kn_inotifyfd);
    if (n < 0)
        return (-1);
    for (off = 0; off + (ssize_t) sizeof(*evt) <= n; off += sizeof(*evt) + evt->len) {
        evt = (struct inotify_event *) (buf + off);
        dbg_printf("inotify event: %s", inotify_event_dump(evt));
        if (evt->len == 0) {
            mask |= evt->mask;
            continue;
        }

        /* A named event is about an entry of the watched directory. Writes
           to and attribute changes of the entries are not events of the
           directory; only adding, removing and renaming entries are. */
        if (!(evt->mask & IN_ENTRY_EVENTS))
            continue;
        entries = 1;
        if (!(evt->mask & IN_ISDIR))
            continue;

        /* Adding or removing a subdirectory changes the directory's link
           count; renaming one within the directory does not. The two
           halves of such a rename are queued next to each other. */
        if ((evt->mask & IN_MOVED_TO) && moved_dir && evt->cookie == moved_cookie) {
            moved_dir = 0;
            continue;
        }
        if (moved_dir)
            subdirs = 1;
        moved_dir = 0;
        if (evt->mask & IN_MOVED_FROM) {
            moved_dir = 1;
            moved_cookie = evt->cookie;
        } else {
            subdirs = 1;
        }
    }
    if (moved_dir)
        subdirs = 1;

    if (mask & IN_IGNORED) {
        /* TODO: possibly return error when fs is unmounted */
        dst->filter = EVFILT_DROP;
        return (0);
    }

    kevent_int_to_64(&src->kev, dst);
    dst->data = 0;

    /* No error checking because fstat(2) should rarely fail */
    //FIXME: EINTR
    attrib = mask & IN_ATTRIB;
    if ((mask & (IN_ATTRIB | IN_MODIFY) || entries)
        && fstat(src->kev.ident, &sb) == 0) {
        if (sb.st_nlink != src->data.vnode.nlink) {
            fflags |= NOTE_LINK;
            /* inotify reports a link count change as IN_ATTRIB; it is an
               attribute change only if something else changed too */
            if (!vnode_attrs_changed(src, &sb))
                attrib = 0;
        }
        /* Removing any name of a file is an unlink() of it, while a
           directory's link count also drops when a subdirectory is
           removed */
        if (sb.st_nlink == 0
            || (!S_ISDIR(sb.st_mode) && sb.st_nlink < src->data.vnode.nlink))
            fflags |= NOTE_DELETE;
#if HAVE_NOTE_TRUNCATE
        if (sb.st_nsize == 0)
            fflags |= NOTE_TRUNCATE;
#endif
        /* A directory grows with its entries, which is reported as
           NOTE_WRITE only */
        if (!S_ISDIR(sb.st_mode) && sb.st_size > src->data.vnode.size)
            fflags |= NOTE_EXTEND;
        vnode_snapshot(src, &sb);
    }

    if (mask & IN_MODIFY || entries)
        fflags |= NOTE_WRITE;
    if (subdirs)
        fflags |= NOTE_LINK;
    if (attrib)
        fflags |= NOTE_ATTRIB;
    if (mask & IN_MOVE_SELF)
        fflags |= NOTE_RENAME;
    if (mask & IN_DELETE_SELF)
        fflags |= NOTE_DELETE;

    /* Report only the requested events that happened (an IN_ATTRIB may be
       seen only because inotify needs a mask, or nothing was queued) */
    dst->fflags = fflags & src->kev.fflags;
    if (dst->fflags == 0)
        dst->filter = EVFILT_DROP;

    return (0);
}

int
evfilt_vnode_knote_create(struct filter *filt, struct knote *kn)
{
    struct stat sb;

    if (fstat(kn->kev.ident, &sb) < 0) {
        dbg_puts("fstat failed");
        return (-1);
    }
    vnode_snapshot(kn, &sb);
    kn->kev.data = -1;

    return (add_watch(filt, kn));
}

int
evfilt_vnode_knote_modify(struct filter *filt, struct knote *kn, 
        const struct kevent64_s *kev)
{
    /* Re-adding the registration replaces the events it watches for. An
       enabled knote gets a new watch with the new mask; a disabled one
       has no watch, and enabling it creates one from the new flags. */
    kn->kev.fflags = kev->fflags;
    if (kn->kev.flags & EV_DISABLE)
        return (0);
    if (delete_watch(filt, kn) < 0)
        return (-1);
    return (add_watch(filt, kn));
}

int
evfilt_vnode_knote_delete(struct filter *filt, struct knote *kn)
{   
    return delete_watch(filt, kn);
}

int
evfilt_vnode_knote_enable(struct filter *filt, struct knote *kn)
{
    return add_watch(filt, kn);
}

int
evfilt_vnode_knote_disable(struct filter *filt, struct knote *kn)
{
    return delete_watch(filt, kn);
}

const struct filter evfilt_vnode = {
    EVFILT_VNODE,
    NULL,
    NULL,
    evfilt_vnode_copyout,
    evfilt_vnode_knote_create,
    evfilt_vnode_knote_modify,
    evfilt_vnode_knote_delete,
    evfilt_vnode_knote_enable,
    evfilt_vnode_knote_disable,        
};
