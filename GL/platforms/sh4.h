#pragma once

#include <kos.h>
#include <dc/matrix.h>
#include <dc/pvr.h>
#include <dc/vec3f.h>
#include <dc/fmath.h>
#include <dc/matrix3d.h>

#include "../types.h"
#include "../private.h"

#ifdef USE_SH4ZAM
#include <sh4zam/shz_xmtrx.h>   /* shz_xmtrx_load/apply/store_4x4 (faster than KOS mat_*) */
#include <sh4zam/shz_mem.h>     /* shz_memcpy — replaces the byte-at-a-time copy loop */
#endif

#ifndef NDEBUG
#define PERF_WARNING(msg) printf("[PERF] %s\n", msg)
#else
#define PERF_WARNING(msg) (void) 0
#endif

#ifndef GL_FORCE_INLINE
#define GL_NO_INSTRUMENT inline __attribute__((no_instrument_function))
#define GL_INLINE_DEBUG GL_NO_INSTRUMENT __attribute__((always_inline))
#define GL_FORCE_INLINE static GL_INLINE_DEBUG
#endif


// ---- sh4_math.h - SH7091 Math Module ----
//
// This file is part of the DreamHAL project, a hardware abstraction library
// primarily intended for use on the SH7091 found in hardware such as the SEGA
// Dreamcast game console.
//
// This math module is hereby released into the public domain in the hope that it
// may prove useful. Now go hit 60 fps! :)
//
// --Moopthehedgehog

// 1/sqrt(x)
GL_FORCE_INLINE float MATH_fsrra(float x)
{
  asm volatile ("fsrra %[one_div_sqrt]\n"
  : [one_div_sqrt] "+f" (x) // outputs, "+" means r/w
  : // no inputs
  : // no clobbers
  );

  return x;
}

// 1/x = 1 / sqrt(x^2)
GL_FORCE_INLINE float MATH_Fast_Invert(float x)
{
  int neg = x < 0.0f;

  x = MATH_fsrra(x * x);

  if (neg) x = -x;
  return x;
}
// end of ---- sh4_math.h ----

#define PREFETCH(addr) __builtin_prefetch((addr))

/* Allocate (validate) the 32-byte cache line at `addr` WITHOUT reading it from RAM
   (SH4 MOVCA.L). For write-only streams whose records are exactly one line (the
   32-byte-aligned Vertex array), this removes a full memory read per record — the
   line would otherwise be fetched just to be overwritten. The word at offset 0 is
   left with garbage (r0): callers MUST overwrite all 32 bytes before relying on
   them (the PUC generators write every field across their passes). */
#define VERTEX_CACHE_ALLOC(addr) \
    __asm__ volatile("movca.l r0, @%0" : : "r"(addr) : "memory")

GL_FORCE_INLINE void* memcpy_fast(void *dest, const void *src, size_t len) {
#ifdef USE_SH4ZAM
  /* sh4zam picks the best aligned / store-queue specialization at runtime (no
     alignment or size requirement), replacing the mov.b one-byte-per-iteration
     loop below. */
  return shz_memcpy(dest, src, len);
#else
  if(!len) {
    return dest;
  }

  const uint8_t *s = (uint8_t *)src;
  uint8_t *d = (uint8_t *)dest;

  uint32_t diff = (uint32_t)d - (uint32_t)(s + 1); // extra offset because input gets incremented before output is calculated
  // Underflow would be like adding a negative offset

  // Can use 'd' as a scratch reg now
  asm volatile (
    "clrs\n" // Align for parallelism (CO) - SH4a use "stc SR, Rn" instead with a dummy Rn
  ".align 2\n"
  "0:\n\t"
    "dt %[size]\n\t" // (--len) ? 0 -> T : 1 -> T (EX 1)
    "mov.b @%[in]+, %[scratch]\n\t" // scratch = *(s++) (LS 1/2)
    "bf.s 0b\n\t" // while(s != nexts) aka while(!T) (BR 1/2)
    " mov.b %[scratch], @(%[offset], %[in])\n" // *(datatype_of_s*) ((char*)s + diff) = scratch, where src + diff = dest (LS 1)
    : [in] "+&r" ((uint32_t)s), [scratch] "=&r" ((uint32_t)d), [size] "+&r" (len) // outputs
    : [offset] "z" (diff) // inputs
    : "t", "memory" // clobbers
  );

  return dest;
#endif
}

/* We use sq_cpy if the src and size is properly aligned. We control that the
 * destination is properly aligned so we assert that. */
#define FASTCPY(dst, src, bytes) \
    do { \
        if(bytes % 32 == 0 && ((uintptr_t) src % 4) == 0) { \
            gl_assert(((uintptr_t) dst) % 32 == 0); \
            sq_cpy(dst, src, bytes); \
        } else { \
            memcpy_fast(dst, src, bytes); \
        } \
    } while(0)


/* MEMCPY4 is only ever called with compile-time-constant sizes (4/8/12/64),
   so __builtin_memcpy lowers each to the minimal inline load/store sequence —
   faster than any runtime-dispatched copy (sh4zam or the byte loop), and it
   needs no alignment guarantee. */
#define MEMCPY4(dst, src, bytes) __builtin_memcpy(dst, src, bytes)

#define MEMSET4(dst, v, size) memset((dst), (v), (size))

#define VEC3_NORMALIZE(x, y, z) vec3f_normalize((x), (y), (z))
#define VEC3_LENGTH(x, y, z, l) vec3f_length((x), (y), (z), (l))
#define VEC3_DOT(x1, y1, z1, x2, y2, z2, d) vec3f_dot((x1), (y1), (z1), (x2), (y2), (z2), (d))

/* SH4ZAM xmtrx load/apply/store: same XMTRX semantics as KOS mat_load/apply/
   store (same fmov.d 8-byte alignment, apply post-multiplies XMTRX), but ~2x
   faster per Falco. Layouts are interchangeable (Matrix4x4 == column-major
   float[16] == shz_mat4x4_t). */
GL_FORCE_INLINE void UploadMatrix4x4(const Matrix4x4* mat) {
#ifdef USE_SH4ZAM
    shz_xmtrx_load_4x4((const shz_mat4x4_t*) mat);
#else
    mat_load((matrix_t*) mat);
#endif
}

GL_FORCE_INLINE void DownloadMatrix4x4(Matrix4x4* mat) {
#ifdef USE_SH4ZAM
    shz_xmtrx_store_4x4((shz_mat4x4_t*) mat);
#else
    mat_store((matrix_t*) mat);
#endif
}

GL_FORCE_INLINE void MultiplyMatrix4x4(const Matrix4x4* mat) {
#ifdef USE_SH4ZAM
    shz_xmtrx_apply_4x4((const shz_mat4x4_t*) mat);
#else
    mat_apply((matrix_t*) mat);
#endif
}

GL_FORCE_INLINE void TransformVec3(float* x) {
    mat_trans_single4(x[0], x[1], x[2], x[3]);
}

/* Transform a 3-element vector using the stored matrix (w == 1) */
GL_FORCE_INLINE void TransformVec3NoMod(const float* xIn, float* xOut) {
    mat_trans_single3_nodiv_nomod(xIn[0], xIn[1], xIn[2], xOut[0], xOut[1], xOut[2]);
}

/* Transform a 3-element normal using the stored matrix (w == 0)*/
GL_FORCE_INLINE void TransformNormalNoMod(const float* in, float* out) {
    mat_trans_normal3_nomod(in[0], in[1], in[2], out[0], out[1], out[2]);
}

/* Transform a 4-element vector in-place by the stored matrix */
inline void TransformVec4(float* x) {

}

GL_FORCE_INLINE void TransformVertex(float x, float y, float z, float w, float* oxyz, float* ow) {
    register float __x __asm__("fr4") = x;
    register float __y __asm__("fr5") = y;
    register float __z __asm__("fr6") = z;
    register float __w __asm__("fr7") = w;

    __asm__ __volatile__(
        "ftrv   xmtrx,fv4\n"
        : "=f" (__x), "=f" (__y), "=f" (__z), "=f" (__w)
        : "0" (__x), "1" (__y), "2" (__z), "3" (__w)
    );

    oxyz[0] = __x;
    oxyz[1] = __y;
    oxyz[2] = __z;
    *ow = __w;
}

/* Dual-vertex transform (2026-07-16, the heavy-city gap): FTRV has ~4-cycle
   issue latency and the single-vertex form serializes load->FTRV->store per
   vertex. Two back-to-back FTRVs on fv4/fv8 let the second issue while the
   first drains, and give GCC eight loads to schedule BEFORE the block and
   eight stores after — the FTRV latency hides under real work instead of
   stalling. w is fixed at 1.0f (every fused-writer caller passes 1.0f). */
GL_FORCE_INLINE void TransformVertex2(
        float x0, float y0, float z0, float* oxyz0, float* ow0,
        float x1, float y1, float z1, float* oxyz1, float* ow1) {
    register float __a0 __asm__("fr4")  = x0;
    register float __a1 __asm__("fr5")  = y0;
    register float __a2 __asm__("fr6")  = z0;
    register float __a3 __asm__("fr7")  = 1.0f;
    register float __b0 __asm__("fr8")  = x1;
    register float __b1 __asm__("fr9")  = y1;
    register float __b2 __asm__("fr10") = z1;
    register float __b3 __asm__("fr11") = 1.0f;

    __asm__ __volatile__(
        "ftrv   xmtrx,fv4\n\t"
        "ftrv   xmtrx,fv8\n"
        : "=f" (__a0), "=f" (__a1), "=f" (__a2), "=f" (__a3),
          "=f" (__b0), "=f" (__b1), "=f" (__b2), "=f" (__b3)
        : "0" (__a0), "1" (__a1), "2" (__a2), "3" (__a3),
          "4" (__b0), "5" (__b1), "6" (__b2), "7" (__b3)
    );

    oxyz0[0] = __a0;
    oxyz0[1] = __a1;
    oxyz0[2] = __a2;
    *ow0 = __a3;
    oxyz1[0] = __b0;
    oxyz1[1] = __b1;
    oxyz1[2] = __b2;
    *ow1 = __b3;
}

/* ---- The GOLD pair writer (2026-07-16): two COMPLETE 32-byte vertex records
   in one scheduled block. The C pair form still serializes FTRV -> result
   stores; here the second vertex's position loads hide the first FTRV's
   latency, the uv loads hide the second's, MOVCA allocates each destination
   line without a read, and the records fill DESCENDING with @-Rn stores
   (flags/x/y/z/u/v/bgra/w — the exact Vertex layout). w is fldi1 (the callers
   always transform w=1). Colors and flags arrive as words so GCC schedules
   their loads before the block. Destinations MUST be 32-byte-aligned Vertex
   records (aligned_vector guarantees it). Pointers are consumed (pass copies). */
#define HAVE_GOLD_PAIR 1
GL_FORCE_INLINE void TransformFillPair(
        const float* p0, const float* p1,
        const float* u0, const float* u1,
        uint32_t c0, uint32_t c1,
        uint32_t f0, uint32_t f1,
        void* da, void* db) {
    __asm__ __volatile__(
        "fldi1   fr7\n\t"
        "fmov.s  @%[p0]+, fr4\n\t"
        "fmov.s  @%[p0]+, fr5\n\t"
        "fmov.s  @%[p0]+, fr6\n\t"
        "fldi1   fr11\n\t"
        "ftrv    xmtrx, fv4\n\t"
        "fmov.s  @%[p1]+, fr8\n\t"      /* v1 loads issue under fv4's FTRV */
        "fmov.s  @%[p1]+, fr9\n\t"
        "fmov.s  @%[p1]+, fr10\n\t"
        "ftrv    xmtrx, fv8\n\t"
        "fmov.s  @%[u0]+, fr0\n\t"      /* uv loads issue under fv8's FTRV */
        "fmov.s  @%[u0]+, fr1\n\t"
        "fmov.s  @%[u1]+, fr2\n\t"
        "fmov.s  @%[u1]+, fr3\n\t"
        "movca.l r0, @%[da]\n\t"        /* allocate da's line, no RAM read */
        "add     #32, %[da]\n\t"
        "fmov.s  fr7,  @-%[da]\n\t"     /* w0  (offset 28) */
        "mov.l   %[c0], @-%[da]\n\t"    /* bgra0 (24) */
        "fmov.s  fr1,  @-%[da]\n\t"     /* v0  (20) */
        "fmov.s  fr0,  @-%[da]\n\t"     /* u0  (16) */
        "fmov.s  fr6,  @-%[da]\n\t"     /* z0  (12) */
        "fmov.s  fr5,  @-%[da]\n\t"     /* y0  (8)  */
        "fmov.s  fr4,  @-%[da]\n\t"     /* x0  (4)  */
        "mov.l   %[f0], @-%[da]\n\t"    /* flags0 (0) */
        "movca.l r0, @%[db]\n\t"
        "add     #32, %[db]\n\t"
        "fmov.s  fr11, @-%[db]\n\t"
        "mov.l   %[c1], @-%[db]\n\t"
        "fmov.s  fr3,  @-%[db]\n\t"
        "fmov.s  fr2,  @-%[db]\n\t"
        "fmov.s  fr10, @-%[db]\n\t"
        "fmov.s  fr9,  @-%[db]\n\t"
        "fmov.s  fr8,  @-%[db]\n"
        "mov.l   %[f1], @-%[db]\n\t"
        : [p0] "+&r" (p0), [p1] "+&r" (p1),
          [u0] "+&r" (u0), [u1] "+&r" (u1),
          [da] "+&r" (da), [db] "+&r" (db)
        : [c0] "r" (c0), [c1] "r" (c1), [f0] "r" (f0), [f1] "r" (f1)
        : "r0", "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7",
          "fr8", "fr9", "fr10", "fr11", "memory"
    );
}

void InitGPU(_Bool autosort, _Bool fsaa);

void ShutdownGPU();

static inline size_t GPUMemoryAvailable() {
    return pvr_mem_available();
}

static inline void* GPUMemoryAlloc(size_t size) {
    return pvr_mem_malloc(size);
}

static inline void GPUSetPaletteFormat(GPUPaletteFormat format) {
    pvr_set_pal_format(format);
}

static inline void GPUSetPaletteEntry(uint32_t idx, uint32_t value) {
    pvr_set_pal_entry(idx, value);
}

static inline void GPUSetBackgroundColour(float r, float g, float b) {
    pvr_set_bg_color(r, g, b);
}

#define PT_ALPHA_REF 0x011c

static inline void GPUSetAlphaCutOff(uint8_t val) {
    PVR_SET(PT_ALPHA_REF, val);
}

static inline void GPUSetClearDepth(float v) {
    pvr_set_zclip(v);
}

static inline void GPUSetFogLinear(float start, float end) {
    pvr_fog_table_linear(start, end);
}

static inline void GPUSetFogExp(float density) {
    pvr_fog_table_exp(density);
}

static inline void GPUSetFogExp2(float density) {
    pvr_fog_table_exp2(density);
}

static inline void GPUSetFogColor(float r, float g, float b, float a) {
    pvr_fog_table_color(r, g, b, a);
}
