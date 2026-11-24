/*
 * Copyright (c) 2026 Cristian Magherusan-Stanciu
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

/*
 * EVFILT_MEMORYSTATUS carries the system memory pressure notifications that
 * back DISPATCH_SOURCE_TYPE_MEMORYPRESSURE. That dispatch source is
 * documented as reporting changes of the memory pressure condition, so a
 * system whose condition never changes reports nothing.
 *
 * This filter accepts, modifies, disables and deletes registrations and
 * never reports an event, as no source of pressure changes is wired up.
 * Reporting real pressure (for example from the host's pressure stall
 * information) is a separate feature: it needs a documented mapping to the
 * NOTE_MEMORYSTATUS_* conditions.
 */

#include "private.h"

static int
evfilt_memorystatus_copyout(struct kevent64_s *dst, struct knote *src, void *ptr)
{
    /* No event source exists, so this is not reached; never report one. */
    kevent_int_to_64(&src->kev, dst);
    dst->filter = EVFILT_DROP;
    return (0);
}

static int
evfilt_memorystatus_knote_create(struct filter *filt, struct knote *kn)
{
    return (0);
}

static int
evfilt_memorystatus_knote_modify(struct filter *filt, struct knote *kn,
        const struct kevent64_s *kev)
{
    return (0);
}

static int
evfilt_memorystatus_knote_delete(struct filter *filt, struct knote *kn)
{
    return (0);
}

static int
evfilt_memorystatus_knote_enable(struct filter *filt, struct knote *kn)
{
    return (0);
}

static int
evfilt_memorystatus_knote_disable(struct filter *filt, struct knote *kn)
{
    return (0);
}

const struct filter evfilt_memorystatus = {
    EVFILT_MEMORYSTATUS,
    NULL,
    NULL,
    evfilt_memorystatus_copyout,
    evfilt_memorystatus_knote_create,
    evfilt_memorystatus_knote_modify,
    evfilt_memorystatus_knote_delete,
    evfilt_memorystatus_knote_enable,
    evfilt_memorystatus_knote_disable,
};
