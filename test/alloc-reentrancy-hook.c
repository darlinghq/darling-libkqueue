#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_replacement, _replacee) \
    __attribute__((used)) static struct { \
        const void *replacement; \
        const void *replacee; \
    } _interpose_##_replacee \
    __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)(unsigned long)&_replacement, \
        (const void *)(unsigned long)&_replacee \
    };

static int hook_ready;
static int hook_null_fd = -1;
static pthread_t hook_owner;

__attribute__((constructor))
static void init_hook(void) {
    hook_null_fd = open("/dev/null", O_RDONLY);
    __atomic_store_n(&hook_ready, 1, __ATOMIC_RELEASE);
}

static void trigger_close_fd(void) {
    if (!__atomic_load_n(&hook_ready, __ATOMIC_ACQUIRE))
        return;
    pthread_t self = pthread_self();
    if (__atomic_load_n(&hook_owner, __ATOMIC_RELAXED) == self)
        return;
    pthread_t expected = (pthread_t)0;
    if (!__atomic_compare_exchange_n(&hook_owner, &expected, self, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    int fd = dup(hook_null_fd >= 0 ? hook_null_fd : 0);
    if (fd >= 0)
        close(fd);
    __atomic_store_n(&hook_owner, (pthread_t)0, __ATOMIC_RELEASE);
}

static void *hook_malloc(size_t size) {
    trigger_close_fd();
    return malloc(size);
}

static void *hook_calloc(size_t count, size_t size) {
    trigger_close_fd();
    return calloc(count, size);
}

static void *hook_realloc(void *ptr, size_t size) {
    trigger_close_fd();
    return realloc(ptr, size);
}

static void hook_free(void *ptr) {
    trigger_close_fd();
    free(ptr);
}

DYLD_INTERPOSE(hook_malloc, malloc)
DYLD_INTERPOSE(hook_calloc, calloc)
DYLD_INTERPOSE(hook_realloc, realloc)
DYLD_INTERPOSE(hook_free, free)
