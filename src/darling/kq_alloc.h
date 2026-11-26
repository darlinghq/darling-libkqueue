#ifndef _DARLING_KQUEUE_ALLOC_H
#define _DARLING_KQUEUE_ALLOC_H

#include <stddef.h>

void *__darling_kqueue_malloc(size_t size);
void *__darling_kqueue_calloc(size_t nmemb, size_t size);
void *__darling_kqueue_realloc(void *ptr, size_t size);
void __darling_kqueue_free(void *ptr);
void __darling_kqueue_alloc_atfork(void);

#endif
