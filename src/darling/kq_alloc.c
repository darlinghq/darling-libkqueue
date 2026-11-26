/*
 * Private mmap-backed allocator for Darling's in-process libkqueue.
 *
 * libsystem_kernel sits below libsystem_malloc and may hold kqueue locks when
 * creating or deleting knotes, so calling into the process allocator (which
 * may be interposed by custom allocators such as mimalloc and call close(2)
 * during thread initialization or shrinking) can re-enter kqueue_closed_fd()
 * and deadlock. Keeping libkqueue's allocations on private anonymous mmaps
 * eliminates both self-deadlocks and allocator lock-order inversions.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include "kq_alloc.h"

#define KQ_ALLOC_MAGIC      0x4B51414CU
#define KQ_ALLOC_NUM_CLASS  7
#define KQ_ALLOC_LARGE      0xFFFFU
#define KQ_ALLOC_CHUNK_SIZE 65536U
#define KQ_ALLOC_PAGE_ALIGN 16384U

struct kq_alloc_hdr {
    size_t user_size;
    size_t map_len;
    uint32_t magic;
    uint32_t gen;
    uint16_t class_idx;
    uint16_t pad;
} __attribute__((aligned(16)));

struct kq_alloc_bucket {
    void *freelist;
    char *slab_cur;
    char *slab_end;
};

static const size_t kq_class_sizes[KQ_ALLOC_NUM_CLASS] = {
    32, 64, 128, 256, 512, 1024, 2048
};

static pthread_mutex_t kq_alloc_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t kq_alloc_gen = 1;
static struct kq_alloc_bucket kq_buckets[KQ_ALLOC_NUM_CLASS];

static int
kq_class_for_size(size_t size)
{
    for (int i = 0; i < KQ_ALLOC_NUM_CLASS; i++) {
        if (size <= kq_class_sizes[i])
            return i;
    }
    return -1;
}

static size_t
kq_align_page(size_t size)
{
    return (size + (KQ_ALLOC_PAGE_ALIGN - 1)) & ~((size_t)KQ_ALLOC_PAGE_ALIGN - 1);
}

static void *
kq_alloc_internal(size_t size, bool zero)
{
    size_t user_size = size;
    size_t alloc_size = size == 0 ? 1 : size;

    int cls = kq_class_for_size(alloc_size);
    if (cls < 0) {
        if (alloc_size > SIZE_MAX - sizeof(struct kq_alloc_hdr) - (KQ_ALLOC_PAGE_ALIGN - 1)) {
            errno = ENOMEM;
            return NULL;
        }
        size_t total = kq_align_page(sizeof(struct kq_alloc_hdr) + alloc_size);
        void *map = mmap(NULL, total, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
        if (map == MAP_FAILED) {
            errno = ENOMEM;
            return NULL;
        }
        struct kq_alloc_hdr *hdr = (struct kq_alloc_hdr *)map;
        hdr->user_size = user_size;
        hdr->map_len = total;
        hdr->magic = KQ_ALLOC_MAGIC;
        hdr->gen = 0;
        hdr->class_idx = KQ_ALLOC_LARGE;
        hdr->pad = 0;
        /* Anonymous mmap returns zero-filled pages. */
        return (void *)(hdr + 1);
    }

    size_t stride = sizeof(struct kq_alloc_hdr) + kq_class_sizes[cls];
    struct kq_alloc_hdr *hdr = NULL;
    bool from_freelist = false;

    pthread_mutex_lock(&kq_alloc_mtx);
    struct kq_alloc_bucket *b = &kq_buckets[cls];

    if (b->freelist != NULL) {
        void *payload = b->freelist;
        b->freelist = *(void **)payload;
        hdr = ((struct kq_alloc_hdr *)payload) - 1;
        from_freelist = true;
    } else {
        if (b->slab_cur == NULL || (size_t)(b->slab_end - b->slab_cur) < stride) {
            void *chunk = mmap(NULL, KQ_ALLOC_CHUNK_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
            if (chunk == MAP_FAILED) {
                pthread_mutex_unlock(&kq_alloc_mtx);
                errno = ENOMEM;
                return NULL;
            }
            b->slab_cur = (char *)chunk;
            b->slab_end = b->slab_cur + KQ_ALLOC_CHUNK_SIZE;
        }
        hdr = (struct kq_alloc_hdr *)b->slab_cur;
        b->slab_cur += stride;
    }

    uint32_t gen = kq_alloc_gen;
    pthread_mutex_unlock(&kq_alloc_mtx);

    hdr->user_size = user_size;
    hdr->map_len = 0;
    hdr->magic = KQ_ALLOC_MAGIC;
    hdr->gen = gen;
    hdr->class_idx = (uint16_t)cls;
    hdr->pad = 0;

    void *payload = (void *)(hdr + 1);
    if (zero && from_freelist)
        memset(payload, 0, alloc_size);

    return payload;
}

void *
__darling_kqueue_malloc(size_t size)
{
    return kq_alloc_internal(size, false);
}

void *
__darling_kqueue_calloc(size_t nmemb, size_t size)
{
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        errno = ENOMEM;
        return NULL;
    }
    return kq_alloc_internal(nmemb * size, true);
}

void
__darling_kqueue_free(void *ptr)
{
    if (ptr == NULL)
        return;

    struct kq_alloc_hdr *hdr = ((struct kq_alloc_hdr *)ptr) - 1;
    assert(hdr->magic == KQ_ALLOC_MAGIC);

    if (hdr->class_idx == KQ_ALLOC_LARGE) {
        size_t map_len = hdr->map_len;
        hdr->magic = 0;
        (void)munmap(hdr, map_len);
        return;
    }

    assert(hdr->class_idx < KQ_ALLOC_NUM_CLASS);

    hdr->magic = 0;
    pthread_mutex_lock(&kq_alloc_mtx);
    if (hdr->gen == kq_alloc_gen) {
        struct kq_alloc_bucket *b = &kq_buckets[hdr->class_idx];
        *(void **)ptr = b->freelist;
        b->freelist = ptr;
    }
    pthread_mutex_unlock(&kq_alloc_mtx);
}

void *
__darling_kqueue_realloc(void *ptr, size_t size)
{
    if (ptr == NULL)
        return __darling_kqueue_malloc(size);
    if (size == 0) {
        __darling_kqueue_free(ptr);
        return NULL;
    }

    struct kq_alloc_hdr *hdr = ((struct kq_alloc_hdr *)ptr) - 1;
    assert(hdr->magic == KQ_ALLOC_MAGIC);

    size_t old_size = hdr->user_size;
    if (hdr->class_idx != KQ_ALLOC_LARGE && size <= kq_class_sizes[hdr->class_idx]) {
        hdr->user_size = size;
        return ptr;
    }

    void *next = __darling_kqueue_malloc(size);
    if (next == NULL)
        return NULL;

    memcpy(next, ptr, old_size < size ? old_size : size);
    __darling_kqueue_free(ptr);
    return next;
}

void
__darling_kqueue_alloc_atfork(void)
{
    pthread_mutex_init(&kq_alloc_mtx, NULL);
    /* Abandon pre-fork slab freelists in case a parent thread forked mid-update. */
    kq_alloc_gen++;
    memset(kq_buckets, 0, sizeof(kq_buckets));
}
