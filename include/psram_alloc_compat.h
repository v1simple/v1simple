#pragma once

#if defined(CONFIG_SPIRAM_SUPPORT)
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *ps_malloc(size_t size);
void *ps_calloc(size_t n, size_t size);
void *ps_realloc(void *ptr, size_t size);

#ifdef __cplusplus
}
#endif
#endif

