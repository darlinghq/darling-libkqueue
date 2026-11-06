#include <pthread.h>
#include <stdlib.h>

#include "listenregistry.h"

static pthread_mutex_t registry_mtx = PTHREAD_MUTEX_INITIALIZER;
static int* registry_vector = NULL;
static size_t registry_vector_capacity = 0;
static size_t registry_vector_size = 0;

#define REGISTRY_VECTOR_INITIAL_CAPACITY (8)

bool __darling_kqueue_get_listen_status(int fd)
{
    bool result = false;
    pthread_mutex_lock(&registry_mtx);

    if (registry_vector != NULL) {
        for (size_t i = 0; i < registry_vector_size; ++i) {
            if (registry_vector[i] == fd)
                result = true;
        }
    }

    pthread_mutex_unlock(&registry_mtx);

    return result;
}

void __darling_kqueue_register_listen(int fd)
{
    pthread_mutex_lock(&registry_mtx);

    if (registry_vector == NULL) {
        registry_vector = malloc(REGISTRY_VECTOR_INITIAL_CAPACITY * sizeof(int));
        if (registry_vector == NULL)
            goto quit;
        registry_vector_capacity = REGISTRY_VECTOR_INITIAL_CAPACITY;
    } else if (registry_vector_size == registry_vector_capacity) {
        int* newPtr = realloc(registry_vector, registry_vector_capacity * 2 * sizeof(int));
        if (newPtr == NULL)
            goto quit;
        registry_vector = newPtr;
        registry_vector_capacity *= 2;
    }

    for (size_t i = 0; i < registry_vector_size; ++i) {
        if (registry_vector[i] == fd)
            goto quit;
    }

    registry_vector[registry_vector_size] = fd;
    ++registry_vector_size;

quit:
    pthread_mutex_unlock(&registry_mtx);
}

void __darling_kqueue_unregister_listen(int fd)
{
    pthread_mutex_lock(&registry_mtx);

    if (registry_vector != NULL && registry_vector_size > 0) {
        for (size_t i = 0; i < registry_vector_size; ) {
            if (registry_vector[i] == fd) {
                registry_vector[i] = registry_vector[registry_vector_size - 1];
                --registry_vector_size;
            } else {
                ++i;
            }
        }
    }

    pthread_mutex_unlock(&registry_mtx);
}

void __darling_kqueue_listenregistry_atfork(void)
{
    pthread_mutex_init(&registry_mtx, NULL);
}
