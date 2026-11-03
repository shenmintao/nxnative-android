/* Freestanding desktop helper: the NDK host clang needs no Windows SDK or CRT. */
#pragma once
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
