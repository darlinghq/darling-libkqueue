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

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "private.h"

#include "alloc.h"

int
knote_init(void)
{
    return 0;
//    return (mem_init(sizeof(struct knote), 1024));
}

/*
 * EV_UDATA_SPECIFIC asks for a separate registration per udata value.
 * This is supported for EVFILT_READ and EVFILT_WRITE, where every knote
 * already owns its own duplicate of the descriptor in epoll.
 */
static int
knote_udata_specific(const struct knote *kn)
{
    return ((kn->kev.filter == EVFILT_READ || kn->kev.filter == EVFILT_WRITE)
            && (kn->kev.flags & EV_UDATA_SPECIFIC));
}

static int
knote_cmp(struct knote *a, struct knote *b)
{
    int rv, as, bs;

    rv = memcmp(&a->kev.ident, &b->kev.ident, sizeof(a->kev.ident));
    if (rv != 0)
        return (rv);

    /* For one ident, the plain registration sorts before the udata-specific
     * ones, which are ordered by udata. */
    as = knote_udata_specific(a);
    bs = knote_udata_specific(b);
    if (as != bs)
        return (as - bs);
    if (as && a->kev.udata != b->kev.udata)
        return ((a->kev.udata < b->kev.udata) ? -1 : 1);
    return (0);
}

RB_GENERATE(knt, knote, kn_entries, knote_cmp)

struct knote *
knote_new(void)
{
	struct knote *res;

    res = calloc(1, sizeof(struct knote));
	if (res == NULL)
        return (NULL);

    res->kn_ref = 1;

    return (res);
}

void
knote_release(struct knote *kn)
{
    assert (kn->kn_ref > 0);

	if (atomic_dec(&kn->kn_ref) == 0) {
        // This can happen if the note never made it into a filter
        if (/*kn->kn_flags & KNFL_KNOTE_DELETED*/ 1) {
            dbg_printf("freeing knote at %p (delayed)", kn);
            LIST_INSERT_HEAD(&kn->kn_kq->kq_tofree, kn, kn_entries2free);
            kn->kev.filter = EVFILT_DROP;
        } else {
            dbg_puts("this should never happen");
        }
    } else {
        dbg_printf("decrementing refcount of knote %p rc=%d", kn, kn->kn_ref);
    }
}

void
knote_insert(struct filter *filt, struct knote *kn)
{
    pthread_rwlock_wrlock(&filt->kf_knote_mtx);
    RB_INSERT(knt, &filt->kf_knote, kn);
    pthread_rwlock_unlock(&filt->kf_knote_mtx);
}

int
knote_delete(struct filter *filt, struct knote *kn)
{
    struct knote query;
    struct knote *tmp;

    if (kn->kn_flags & KNFL_KNOTE_DELETED) {
        dbg_puts("ERROR: double deletion detected");
        return (-1);
    }

    /*
     * Verify that the knote wasn't removed by another
     * thread before we acquired the knotelist lock.
     */
    query.kev = kn->kev;
    pthread_rwlock_wrlock(&filt->kf_knote_mtx);
    tmp = RB_FIND(knt, &filt->kf_knote, &query);
    if (tmp == kn) {
        RB_REMOVE(knt, &filt->kf_knote, kn);
    }
    pthread_rwlock_unlock(&filt->kf_knote_mtx);

    filt->kn_delete(filt, kn); //XXX-FIXME check return value

    kn->kn_flags |= KNFL_KNOTE_DELETED;

    knote_release(kn);

    return (0);
}

struct knote *
knote_lookup(struct filter *filt, const struct kevent64_s *kev)
{
    struct knote query;
    struct knote *ent = NULL;

    memset(&query, 0, sizeof(query));
    query.kev.ident = kev->ident;
    query.kev.filter = filt->kf_id;
    query.kev.flags = kev->flags & EV_UDATA_SPECIFIC;
    query.kev.udata = kev->udata;

    pthread_rwlock_rdlock(&filt->kf_knote_mtx);
    ent = RB_FIND(knt, &filt->kf_knote, &query);
    pthread_rwlock_unlock(&filt->kf_knote_mtx);

    dbg_printf("id=%" PRIuPTR " ent=%p", (uintptr_t) kev->ident, ent);

    return (ent);
}

/* Delete every registration of ident in this filter, whatever its udata */
void
knote_delete_ident(struct filter *filt, uintptr_t ident)
{
    struct knote query;
    struct knote *kn;

    memset(&query, 0, sizeof(query));
    query.kev.ident = ident;
    query.kev.filter = filt->kf_id;

    for (;;) {
        /* The plain registration sorts first, so this finds the lowest
         * knote for ident if there is any. */
        pthread_rwlock_rdlock(&filt->kf_knote_mtx);
        kn = RB_NFIND(knt, &filt->kf_knote, &query);
        pthread_rwlock_unlock(&filt->kf_knote_mtx);

        if (kn == NULL || kn->kev.ident != ident)
            break;
        if (knote_delete(filt, kn) < 0)
            break;
    }
}
    
#if DEADWOOD
struct knote *
knote_get_by_data(struct filter *filt, intptr_t data)
{
    struct knote *kn;

    pthread_rwlock_rdlock(&filt->kf_knote_mtx);
    RB_FOREACH(kn, knt, &filt->kf_knote) {
        if (data == kn->kev.data) 
            break;
    }
    if (kn != NULL) {
        knote_retain(kn);
    }
    pthread_rwlock_unlock(&filt->kf_knote_mtx);

    return (kn);
}
#endif

int
knote_disable(struct filter *filt, struct knote *kn)
{
    assert(!(kn->kev.flags & EV_DISABLE));

    filt->kn_disable(filt, kn); //TODO: Error checking
    KNOTE_DISABLE(kn);
    return (0);
}

//TODO: knote_enable()

void
knote_delete_all(struct filter *filt)
{
    struct knote *kn, *next;

    /* knote_delete() does not free the knote, so next stays valid */
    for (kn = RB_MIN(knt, &filt->kf_knote); kn != NULL; kn = next) {
        next = RB_NEXT(knt, &filt->kf_knote, kn);
        knote_delete(filt, kn);
    }
}

