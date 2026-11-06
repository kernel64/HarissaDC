// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMU_TYPES_H
#define EMU_TYPES_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef float f32;
typedef double f64;

#if defined(_XBOX) || defined(__BIG_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define EMU_HOST_BIG_ENDIAN 1
#else
#define EMU_HOST_BIG_ENDIAN 0
#endif

#define EMU_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define EMU_CACHE_LINE 128
#ifdef _MSC_VER
#define EMU_NOINLINE __declspec(noinline)
#else
#define EMU_NOINLINE __attribute__((noinline))
#endif
#ifdef _MSC_VER
#define EMU_INLINE __forceinline
#else
#define EMU_INLINE inline __attribute__((always_inline))
#endif
#ifdef _MSC_VER
#define EMU_CACHE_ALIGNED __declspec(align(128))
#else
#define EMU_CACHE_ALIGNED __attribute__((aligned(128)))
#endif

#if !defined(_MSC_VER)
#include <stdio.h>
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strtoui64 strtoull
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define __inline inline
#endif

#endif
