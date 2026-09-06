#include <float.h>
#include <limits.h>
#include <malloc.h>
#include <stdlib.h>

#include <dc/sq.h>
#include <arch/timer.h>

#include "../platform.h"
#include "../config.h"
#include "sh4.h"
#include "../gldc_stats.h"

#if GLDC_S3_SEGMENTED_OP
/* S3 segmented hot drain — machinery at the end of this file. */
void _glS3DrainOP(void);
#endif

#define CLIP_DEBUG 0

/* Shared PVR vertex buffer (all lists, one frame). Enlarged from the stock 2560*256
   (655,360 B = 20,480 verts): the HyperSolar city stage submits its whole geometry TWICE
   under CITY_DOUBLE_PASS (opaque base + punch-through windows), which overran the old buffer
   and stalled the TA -> hard hang / dcload reset. 6144*256 = 1,572,864 B = 49,152 verts holds
   2x the capped city (CITY_SUBMIT_VCAP) plus the rest of the scene. Costs ~917 KB extra VRAM
   over stock (of 8 MB; framebuffers + VQ textures leave room). The game also caps the city
   submission as a hard backstop, so this size is headroom, not a hard dependency. */
#define PVR_VERTEX_BUF_SIZE (6144 * 256)
#define PVR_OPB_COUNT       4

#define likely(x)      __builtin_expect(!!(x), 1)
#define unlikely(x)    __builtin_expect(!!(x), 0)

typedef enum {
    DEFERRED_FOG_NONE = 0,
    DEFERRED_FOG_LINEAR,
    DEFERRED_FOG_FLAT,
    DEFERRED_FOG_EXP2
} DeferredFogMode;

static struct {
    DeferredFogMode mode;
    bool vertex_color_dirty;
    float amount;
    float a;
    float r;
    float g;
    float b;
    float start;
    float end;
    float far_depth;
    float vertex_r;
    float vertex_g;
    float vertex_b;
} deferredFog;

void APIENTRY glKosQueueFogVertexColor(GLfloat r, GLfloat g, GLfloat b) {
    _glSetRadialVertexFogColor(r, g, b);
    deferredFog.vertex_color_dirty = true;
    deferredFog.vertex_r = r;
    deferredFog.vertex_g = g;
    deferredFog.vertex_b = b;
}

void APIENTRY glKosQueueFogTableLinear(GLfloat a, GLfloat r, GLfloat g, GLfloat b,
                                       GLfloat start, GLfloat end) {
    deferredFog.mode = DEFERRED_FOG_LINEAR;
    deferredFog.a = a;
    deferredFog.r = r;
    deferredFog.g = g;
    deferredFog.b = b;
    deferredFog.start = start;
    deferredFog.end = end;
}

void APIENTRY glKosQueueFogTableFlat(GLfloat amount, GLfloat a, GLfloat r, GLfloat g,
                                     GLfloat b, GLfloat farDepth) {
    deferredFog.mode = DEFERRED_FOG_FLAT;
    deferredFog.amount = amount;
    deferredFog.a = a;
    deferredFog.r = r;
    deferredFog.g = g;
    deferredFog.b = b;
    deferredFog.far_depth = farDepth;
}

/* "Exp2"-style distance fog that ACTUALLY matches the scene scale. KOS's pvr_fog_table_exp2()
   hardcodes pvr_fog_far_depth(260), which fights any scene whose draw distance isn't ~260 (the city's
   is much larger) -> fog vanishes. So instead we reuse pvr_fog_table_linear's proven perspective slot
   mapping (far depth = end, the known-good scale) but ease each per-slot fog fraction f -> f^power.
   `power` is a RUNTIME arg (the curve shape lives in the caller, NOT here): <1 thicker-earlier, 1
   linear, >1 thicker-later — so retuning the curve never rebuilds GLdc. start/end are world/eye units
   like glKosQueueFogTableLinear. */
void APIENTRY glKosQueueFogTableExp2(GLfloat a, GLfloat r, GLfloat g, GLfloat b,
                                     GLfloat start, GLfloat end, GLfloat power) {
    deferredFog.mode = DEFERRED_FOG_EXP2;
    deferredFog.a = a;
    deferredFog.r = r;
    deferredFog.g = g;
    deferredFog.b = b;
    deferredFog.start = start;
    deferredFog.end = end;
    deferredFog.amount = power;   /* reuse 'amount' to carry the curve exponent */
}

/* Per-slot perspective fog fraction, identical to KOS's internal inverse_w_depth[]
   (pvr_fog_tables.h): inverse_w_depth[i] = 1/t with t = 2^(j>>4) * ((j&0xf)+16)/16
   for j = i+1. Embedded here so the eased table is self-contained (no dependency on
   the unexported KOS symbol). i in [0,127]. */
GL_FORCE_INLINE float fog_invw_depth(int i) {
    int j = i + 1;
    float t = (float)(1u << (j >> 4)) * (float)((j & 0xf) + 16) * 0.0625f;
    return 1.0f / t;
}

static void ApplyDeferredFogTable(void) {
    /* KOS leaves pvr_fog_vertex_color() unimplemented, but the register is
       ordinary scene-global PVR state. Queue it alongside table fog so the
       write happens only after pvr_wait_ready(), before scene begin. */
    if(deferredFog.vertex_color_dirty) {
        PVR_SET(PVR_FOG_VERTEX_COLOR,
                PVR_PACK_COLOR(1.0f, deferredFog.vertex_r,
                               deferredFog.vertex_g, deferredFog.vertex_b));
        deferredFog.vertex_color_dirty = false;
    }

    if(deferredFog.mode == DEFERRED_FOG_LINEAR) {
        pvr_fog_table_color(deferredFog.a, deferredFog.r, deferredFog.g, deferredFog.b);
        pvr_fog_table_linear(deferredFog.start, deferredFog.end);
    } else if(deferredFog.mode == DEFERRED_FOG_FLAT) {
        float table[129];
        for(int i = 0; i < 129; i++) {
            table[i] = deferredFog.amount;
        }
        pvr_fog_far_depth(deferredFog.far_depth);
        pvr_fog_table_color(deferredFog.a, deferredFog.r, deferredFog.g, deferredFog.b);
        pvr_fog_table_custom(table);
    } else if(deferredFog.mode == DEFERRED_FOG_EXP2) {
        /* Eased distance fog. We mirror pvr_fog_table_linear's EXACT register layout (its perspective
           1/W slot mapping is the only fill that actually renders; a naive depth-indexed table lands in
           the wrong slots and shows nothing) but ease each per-slot linear fog fraction f -> f^power.
           far depth = end keeps the known-good scale; pvr_fog_table_exp2's hardcoded far_depth(260) is
           avoided. `power` is the caller's runtime curve knob: <1 thicker-earlier (concave), 1 = the
           plain linear fill, >1 thicker-later (convex). */
        float start = deferredFog.start < 0.0f ? -deferredFog.start : deferredFog.start;
        float end   = deferredFog.end   < 0.0f ? -deferredFog.end   : deferredFog.end;
        if(start >= end) { deferredFog.mode = DEFERRED_FOG_NONE; return; }
        float power = deferredFog.amount;
        if(power < 0.01f) power = 0.01f;   /* guard: power<=0 would make every slot fully fogged */

        uint32_t table_start = (uint32_t)((start / end) * 128.0f);   /* slots cleared near the eye */
        uint32_t non_zero_entries = 128 - table_start;
        uint32_t step_size = 128 / non_zero_entries;                 /* stretch fill across the table */

        /* table[0] = farthest = full occlusion (= linear's initial valh); table[j+1] = register j. */
        float table[129];
        table[0] = 1.0f;
        for(uint32_t j = 0; j < 128; j++) {
            uint32_t tdx = 127 - j;
            if(tdx >= table_start) {
                float f = fog_invw_depth((int)(j * step_size));   /* linear perspective fraction */
                table[j + 1] = __builtin_powf(f, power);          /* eased by the caller's curve knob */
            } else {
                table[j + 1] = 0.0f;
            }
        }

        pvr_fog_far_depth(end);
        pvr_fog_table_color(deferredFog.a, deferredFog.r, deferredFog.g, deferredFog.b);
        pvr_fog_table_custom(table);
    }

    deferredFog.mode = DEFERRED_FOG_NONE;
}

static void __attribute__((noreturn)) _glSubmissionFatal(void) {
    /* Keep opt-in submission failures on HyperSolar's resolvable guru path;
       plain abort()/exit() may drop silently back to firmware on hardware. */
    gl_assert(false);
    __builtin_unreachable();
}

#if GLDC_N4_VERTEX_DMA
/* ---- N4: GLdc-owned direct KOS vertex-DMA buffers -----------------------

   KOS splits every registered list buffer into two equal frame halves.  GLdc
   grows each list independently from the conservative record budget computed
   before scene begin, then writes final TA records directly into the inactive
   half.  The buffers never shrink, so ordinary frames stop touching the heap
   once the scene's high-water marks have been learned.

   Two records are kept beyond GLdc's payload. Current KOS appends a blank
   header to a DMA list at scene finish and then its zero/EOL marker; retaining
   both also remains safe if KOS later starts marking explicitly-finished DMA
   lists closed and needs only the marker. */
#define GLDC_N4_KOS_TAIL_RECORDS 2u
#define GLDC_N4_GROW_GRANULARITY 4096u

typedef struct GLdcN4DmaBuffer {
    void* allocation;
    size_t frame_bytes;
} GLdcN4DmaBuffer;

static GLdcN4DmaBuffer n4_dma_buffers[3];
static GPUList n4_output_list = GPU_LIST_OP_POLY;
static uintptr_t n4_output_cursor;
static uintptr_t n4_output_limit;
static uintptr_t n4_output_start;
static bool n4_output_open;
static bool n4_submission_registers_initialized;

static const GPUList n4_dma_lists[3] = {
    GPU_LIST_OP_POLY,
    GPU_LIST_PT_POLY,
    GPU_LIST_TR_POLY
};

static int _glN4ListSlot(GPUList list) {
    if(list == GPU_LIST_OP_POLY) return 0;
    if(list == GPU_LIST_PT_POLY) return 1;
    if(list == GPU_LIST_TR_POLY) return 2;
    return -1;
}

static size_t _glN4RoundFrameBytes(size_t payload_records) {
    if(payload_records > (SIZE_MAX / sizeof(Vertex)) -
                         GLDC_N4_KOS_TAIL_RECORDS) {
        fprintf(stderr, "GLdc N4: record budget overflow\n");
        _glSubmissionFatal();
    }

    size_t bytes = (payload_records + GLDC_N4_KOS_TAIL_RECORDS) *
                   sizeof(Vertex);
    if(bytes < 64u) bytes = 64u;
    const size_t remainder = bytes & (GLDC_N4_GROW_GRANULARITY - 1u);
    if(remainder) {
        const size_t add = GLDC_N4_GROW_GRANULARITY - remainder;
        if(bytes > SIZE_MAX - add) {
            fprintf(stderr, "GLdc N4: byte budget overflow\n");
            _glSubmissionFatal();
        }
        bytes += add;
    }
    return bytes;
}

static bool _glN4BuffersNeedGrowth(
        size_t op_records, size_t pt_records, size_t tr_records) {
    const size_t records[3] = {op_records, pt_records, tr_records};
    for(int i = 0; i < 3; ++i) {
        if(_glN4RoundFrameBytes(records[i]) >
           n4_dma_buffers[i].frame_bytes) return true;
    }
    return false;
}

/* Must run only after the previous TA registration has completed. Grow one
   list at a time and free its prior pair immediately; retaining every old and
   replacement buffer until an atomic commit would create a multi-megabyte
   transient heap peak on 16 MiB hardware. Allocation failure is fatal in this
   opt-in lane: silently mixing a too-small DMA list with SQ submission would
   corrupt list ownership and is strictly worse than stopping at the cause. */
static void _glN4EnsureBuffers(
        size_t op_records, size_t pt_records, size_t tr_records) {
    const size_t records[3] = {op_records, pt_records, tr_records};
    bool changed = false;

    for(int i = 0; i < 3; ++i) {
        const GPUList list = n4_dma_lists[i];
        const size_t required = _glN4RoundFrameBytes(records[i]);
        if(required <= n4_dma_buffers[i].frame_bytes) continue;

        size_t grown = required;
        if(n4_dma_buffers[i].frame_bytes) {
            const size_t old = n4_dma_buffers[i].frame_bytes;
            const size_t geometric = old <= SIZE_MAX - old / 2u
                ? old + old / 2u : SIZE_MAX;
            if(geometric > grown) grown = geometric;
            if(grown > SIZE_MAX - (GLDC_N4_GROW_GRANULARITY - 1u)) {
                fprintf(stderr, "GLdc N4: DMA growth size overflow\n");
                _glSubmissionFatal();
            }
            const size_t remainder = grown &
                (GLDC_N4_GROW_GRANULARITY - 1u);
            if(remainder) grown += GLDC_N4_GROW_GRANULARITY - remainder;
        }
        if(grown > SIZE_MAX / 2u) {
            fprintf(stderr, "GLdc N4: DMA allocation size overflow\n");
            _glSubmissionFatal();
        }

        void* const replacement = memalign(32u, grown * 2u);
        if(!replacement) {
            fprintf(stderr,
                    "GLdc N4: unable to allocate %lu-byte double DMA buffer"
                    " for list %d\n",
                    (unsigned long)(grown * 2u), (int)list);
            _glSubmissionFatal();
        }
        void* const old = pvr_set_vertbuf(
            (pvr_list_t)list, replacement, grown * 2u);
        gl_assert(old == n4_dma_buffers[i].allocation);
        free(old);
        n4_dma_buffers[i].allocation = replacement;
        n4_dma_buffers[i].frame_bytes = grown;
        changed = true;
    }

    if(changed) {
        const size_t op = n4_dma_buffers[0].frame_bytes;
        const size_t pt = n4_dma_buffers[1].frame_bytes;
        const size_t tr = n4_dma_buffers[2].frame_bytes;
        fprintf(stderr,
                "[GLDC-N4] DMA KiB/frame op=%lu pt=%lu tr=%lu"
                " (double-buffered total=%lu KiB)\n",
                (unsigned long)(op >> 10),
                (unsigned long)(pt >> 10),
                (unsigned long)(tr >> 10),
                (unsigned long)((2u * (op + pt + tr)) >> 10));
    }
}

static bool _glN4FogPending(void) {
    return deferredFog.vertex_color_dirty ||
           deferredFog.mode != DEFERRED_FOG_NONE;
}

static void _glN4ShutdownBuffers(void) {
    for(int i = 0; i < 3; ++i) {
        free(n4_dma_buffers[i].allocation);
        n4_dma_buffers[i].allocation = NULL;
        n4_dma_buffers[i].frame_bytes = 0u;
    }
    n4_output_cursor = n4_output_limit = n4_output_start = 0u;
    n4_output_open = false;
    n4_submission_registers_initialized = false;
}
#endif

void InitGPU(_Bool autosort, _Bool fsaa) {
    pvr_init_params_t params = {
        /* Bin sizes: opaque, op_modifier, translucent, tr_modifier, punch-through.
           KOS caps bin size at _32. */
        {PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32},
        PVR_VERTEX_BUF_SIZE, /* Vertex buffer size */
        GLDC_N4_VERTEX_DMA, /* N4: KOS list-major vertex DMA */
        fsaa, /* No FSAA */
        (autosort) ? 0 : 1, /* Disable translucent auto-sorting to match traditional GL */
        PVR_OPB_COUNT, /* Number of tile object pointer overflow bins. */
        0 /* Keep KOS's generated vertex buffer double-buffered. */
    };

    pvr_init(&params);

#ifndef _arch_sub_naomi
    /* If we're PAL and we're NOT VGA, then use 50hz by default. This is the safest
    thing to do. If someone wants to force 60hz then they can call vid_set_mode later and hopefully
    that'll work... */

    int cable = vid_check_cable();

    if(cable != CT_VGA) {
        int region = flashrom_get_region();
        if (region == FLASHROM_REGION_EUROPE) {
            printf("PAL region without VGA - enabling 50hz");
            vid_set_mode(DM_640x480_PAL_IL, PM_RGB565);
        }
    }
#endif
}

void ShutdownGPU() {
    pvr_shutdown();
#if GLDC_N4_VERTEX_DMA
    _glN4ShutdownBuffers();
#endif
}

GL_FORCE_INLINE float _glFastInvert(float x) {
    /* 1/|x| via FSRRA (~3 cycles) instead of fsqrt+fdiv (~40): this is the single
       hottest per-vertex op at flush (2026-07-15 HyperSolar audit). Same 1/|x|
       semantics as the old 1.0f / sqrtf(x*x) — the sign is dropped either way. */
    return MATH_fsrra(x * x);
}

GL_FORCE_INLINE void _glPerspectiveDivideVertex(Vertex* vertex, int count) {
    TRACE();

    for(int v = 0; v < count; ++v) {
        const float f = _glFastInvert(vertex[v].w);

        /* Convert to screenspace */
        /* (note that vertices have already been viewport transformed) */
        vertex[v].xyz[0] *= f;
        vertex[v].xyz[1] *= f;

        /* Orthographic projections need to use invZ otherwise we lose
        the depth information. As w == 1, and clip-space range is -w to +w
        we add 1.0 to the Z to bring it into range. We add a little extra to
        avoid a divide by zero.
        */
        if(vertex[v].w == 1.0f) {
            vertex[v].xyz[2] = _glFastInvert(1.0001f + vertex[v].xyz[2]);
        } else {
            vertex[v].xyz[2] = f;
        }
    }
}

/* N1 proved this RAM finalizer byte-exact against the classic path. N3 makes
   that kernel permanent: transient callers write final records into GLdc-owned
   packet RAM, while the benchmark and production lane share this one body. */
typedef char FinalP3T2BGRAInputSizeMustBe24[
    sizeof(GLKosVertexP3T2BGRA) == 24 ? 1 : -1];

#if defined(GLDC_NATIVE_BENCH) && GLDC_NATIVE_BENCH
/* Conservative input-side guard for this fixed packed-color benchmark. KOS's
   current non-DMA close sequence adds eight TA input records with InitGPU's
   OP/TR/PT configuration: pvr_list_finish(OP) appends a blank header + EOL,
   then pvr_scene_finish() closes each unopened TR/PT list with two blank
   headers + EOL. PVR_VERTEX_BUF_SIZE is generated ISP/TSP parameter storage,
   not a universal count of 32-byte inputs; the hardware harness separately
   records that generated footprint and uses only the ~30k-record workloads. */
#define NATIVE_BENCH_TA_CLOSE_RECORDS 8
#define NATIVE_BENCH_INPUT_RECORD_LIMIT \
    ((PVR_VERTEX_BUF_SIZE / (int)sizeof(Vertex)) - \
     NATIVE_BENCH_TA_CLOSE_RECORDS)
#endif

/* The sole final-record fill body. RAM and store-queue sinks differ only in
   cache-line allocation / queue firing around this function, keeping every
   word and every SH4 reciprocal operation identical. */
GL_FORCE_INLINE uint32_t _glFinalFloatWord(float value) {
    uint32_t bits;
    __builtin_memcpy(&bits, &value, sizeof(bits));
    return bits;
}

GL_FORCE_INLINE bool _glFinalWordFinite(uint32_t bits) {
    return (bits & 0x7f800000u) != 0x7f800000u;
}

GL_FORCE_INLINE bool _glFinalFloatFinite(float value) {
    return _glFinalWordFinite(_glFinalFloatWord(value));
}

GL_FORCE_INLINE bool _glFinalWordPositiveFinite(uint32_t bits) {
    const uint32_t magnitude = bits & 0x7fffffffu;
    return ((bits >> 31) == 0u) & (magnitude != 0u) &
           ((magnitude & 0x7f800000u) != 0x7f800000u);
}

GL_FORCE_INLINE int _glFinalFillRecord(
        const GLKosVertexP3T2BGRA* in, uint32_t* q, uint32_t flags,
        float x, float y, float z, float w) {
    int result = (z >= -w) ? SCENE_FINAL_BUILD_OK
                           : SCENE_FINAL_BUILD_NEAR;
    const float f = _glFastInvert(w);
    q[0] = flags;
    ((float*)q)[1] = x * f;
    ((float*)q)[2] = y * f;
    ((float*)q)[3] = unlikely(w == 1.0f)
        ? _glFastInvert(1.0001f + z)
        : f;
    ((float*)q)[4] = in->u;
    ((float*)q)[5] = in->v;
    q[6] = in->bgra;
    ((float*)q)[7] = w;

    /* These are exactly the finite fields the TA consumes and the public raw
       packet API validates.  z is also checked because it owns near-plane
       classification but is not otherwise retained in the final record.
       Bitwise OR keeps one cold INVALID decision under -ffast-math. */
    const bool invalid =
        !_glFinalFloatFinite(z) |
        !_glFinalWordFinite(q[1]) |
        !_glFinalWordFinite(q[2]) |
        !_glFinalWordPositiveFinite(q[3]) |
        !_glFinalWordFinite(q[4]) |
        !_glFinalWordFinite(q[5]);
    result |= ((int)invalid << 1);
    return result;
}

GL_FORCE_INLINE int _glFinalPack(
        const GLKosVertexP3T2BGRA* in, Vertex* out, uint32_t flags,
        float x, float y, float z, float w) {
    VERTEX_CACHE_ALLOC(out);
    return _glFinalFillRecord(in, (uint32_t*)out, flags, x, y, z, w);
}

GL_FORCE_INLINE int _glFinalPackPair(
        const GLKosVertexP3T2BGRA* a, const GLKosVertexP3T2BGRA* b,
        Vertex* da, Vertex* db, uint32_t fa, uint32_t fb) {
    float axyz[3], bxyz[3], aw, bw;
    TransformVertex2(a->x, a->y, a->z, axyz, &aw,
                     b->x, b->y, b->z, bxyz, &bw);
    const int ar = _glFinalPack(
        a, da, fa, axyz[0], axyz[1], axyz[2], aw);
    const int br = _glFinalPack(
        b, db, fb, bxyz[0], bxyz[1], bxyz[2], bw);
    return ar | br;
}

GL_FORCE_INLINE int _glFinalPackSingle(
        const GLKosVertexP3T2BGRA* in, Vertex* out, uint32_t flags) {
    float xyz[3], w;
    TransformVertex(in->x, in->y, in->z, 1.0f, xyz, &w);
    return _glFinalPack(in, out, flags, xyz[0], xyz[1], xyz[2], w);
}

int SceneBuildFinalP3T2BGRA(
        unsigned int mode, const void* vertices, int count, Vertex* output) {
    const GLKosVertexP3T2BGRA* in =
        (const GLKosVertexP3T2BGRA*)vertices;

    if(mode == GL_TRIANGLES) {
        int i = 0;
        /* Six records = two complete triangles. Fixed flags eliminate the
           old loop-carried modulo/EOL decision and retain dual-FTRV issue. */
        for(; count - i >= 6; i += 6) {
            if(count - i > 2) PREFETCH(in + i + 2);
            const int r0 = _glFinalPackPair(
                in + i, in + i + 1, output + i, output + i + 1,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX);
            const int r1 = _glFinalPackPair(
                in + i + 2, in + i + 3, output + i + 2, output + i + 3,
                GPU_CMD_VERTEX_EOL, GPU_CMD_VERTEX);
            const int r2 = _glFinalPackPair(
                in + i + 4, in + i + 5, output + i + 4, output + i + 5,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL);
            /* A rejected reservation is cancelled wholesale.  Stop at the
               first complete unrolled block whose records cannot be queued;
               later output would be unobservable and only delays F1. */
            const int block_result = r0 | r1 | r2;
            if(unlikely(block_result != SCENE_FINAL_BUILD_OK))
                return block_result;
        }
        if(i < count) {  /* valid triangle input leaves exactly three */
            if(count - i > 2) PREFETCH(in + i + 2);
            const int pair_result = _glFinalPackPair(
                in + i, in + i + 1, output + i, output + i + 1,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX);
            return pair_result | _glFinalPackSingle(
                in + i + 2, output + i + 2, GPU_CMD_VERTEX_EOL);
        }
        return SCENE_FINAL_BUILD_OK;
    }

    if(mode == GL_QUADS) {
        for(int i = 0; i < count; i += 4) {
            if(count - i > 4) PREFETCH(in + i + 4);
            const int r0 = _glFinalPackPair(
                in + i, in + i + 1, output + i, output + i + 1,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX);
            const int r1 = _glFinalPackPair(
                in + i + 3, in + i + 2, output + i + 2, output + i + 3,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL);
            const int block_result = r0 | r1;
            if(unlikely(block_result != SCENE_FINAL_BUILD_OK))
                return block_result;
        }
        return SCENE_FINAL_BUILD_OK;
    }

    /* Long strips are their own benchmark topology. One EOL is stamped on
       the final record, matching _glWriteFusedVertices(GL_FALSE). */
    int i = 0;
    for(; count - i >= 2; i += 2) {
        if(count - i > 2) PREFETCH(in + i + 2);
        const uint32_t fb = (count - i == 2)
            ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX;
        const int pair_result = _glFinalPackPair(
            in + i, in + i + 1, output + i, output + i + 1,
            GPU_CMD_VERTEX, fb);
        if(unlikely(pair_result != SCENE_FINAL_BUILD_OK))
            return pair_result;
    }
    if(i < count) {
        return _glFinalPackSingle(in + i, output + i, GPU_CMD_VERTEX_EOL);
    }
    return SCENE_FINAL_BUILD_OK;
}

/* Trusted N3 record writer shared by production and the retained N1 hardware
   control. It intentionally omits the checked constructor's per-record finite
   scan: callers own the documented finite-input/matrix/depth contract. Near
   classification remains inside this writer so an ambiguous batch can cancel
   its private reservation and take the exact clipping fallback. */
GL_FORCE_INLINE bool _glTrustedFinalFillRecord(
        const GLKosVertexP3T2BGRA* in, uint32_t* q, uint32_t flags,
        float x, float y, float z, float w) {
    const bool visible = z >= -w;
    const float f = _glFastInvert(w);
    q[0] = flags;
    ((float*)q)[1] = x * f;
    ((float*)q)[2] = y * f;
    ((float*)q)[3] = unlikely(w == 1.0f)
        ? _glFastInvert(1.0001f + z)
        : f;
    ((float*)q)[4] = in->u;
    ((float*)q)[5] = in->v;
    q[6] = in->bgra;
    ((float*)q)[7] = w;
    return visible;
}

GL_FORCE_INLINE bool _glTrustedFinalPack(
        const GLKosVertexP3T2BGRA* in, Vertex* out, uint32_t flags,
        float x, float y, float z, float w) {
    VERTEX_CACHE_ALLOC(out);
    return _glTrustedFinalFillRecord(
        in, (uint32_t*)out, flags, x, y, z, w);
}

GL_FORCE_INLINE bool _glTrustedFinalPackPair(
        const GLKosVertexP3T2BGRA* a, const GLKosVertexP3T2BGRA* b,
        Vertex* da, Vertex* db, uint32_t fa, uint32_t fb) {
    float axyz[3], bxyz[3], aw, bw;
    TransformVertex2(a->x, a->y, a->z, axyz, &aw,
                     b->x, b->y, b->z, bxyz, &bw);
    const bool av = _glTrustedFinalPack(a, da, fa,
                                       axyz[0], axyz[1], axyz[2], aw);
    const bool bv = _glTrustedFinalPack(b, db, fb,
                                       bxyz[0], bxyz[1], bxyz[2], bw);
    return av && bv;
}

GL_FORCE_INLINE bool _glTrustedFinalPackSingle(
        const GLKosVertexP3T2BGRA* in, Vertex* out, uint32_t flags) {
    float xyz[3], w;
    TransformVertex(in->x, in->y, in->z, 1.0f, xyz, &w);
    return _glTrustedFinalPack(in, out, flags,
                              xyz[0], xyz[1], xyz[2], w);
}

#if defined(GLDC_NATIVE_BENCH) && GLDC_NATIVE_BENCH
int SceneNativeBenchBuildP3T2BGRA(
        unsigned int mode, const void* vertices, int count, Vertex* output) {
    const GLKosVertexP3T2BGRA* in =
        (const GLKosVertexP3T2BGRA*)vertices;

    if(mode == GL_TRIANGLES) {
        bool all_visible = true;
        int i = 0;
        for(; count - i >= 6; i += 6) {
            if(count - i > 2) PREFETCH(in + i + 2);
            all_visible &= _glTrustedFinalPackPair(
                in + i, in + i + 1, output + i, output + i + 1,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX);
            all_visible &= _glTrustedFinalPackPair(
                in + i + 2, in + i + 3, output + i + 2, output + i + 3,
                GPU_CMD_VERTEX_EOL, GPU_CMD_VERTEX);
            all_visible &= _glTrustedFinalPackPair(
                in + i + 4, in + i + 5, output + i + 4, output + i + 5,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL);
        }
        if(i < count) {
            if(count - i > 2) PREFETCH(in + i + 2);
            all_visible &= _glTrustedFinalPackPair(
                in + i, in + i + 1, output + i, output + i + 1,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX);
            all_visible &= _glTrustedFinalPackSingle(
                in + i + 2, output + i + 2, GPU_CMD_VERTEX_EOL);
        }
        return all_visible ? GL_KOS_NATIVE_BENCH_OK
                           : GL_KOS_NATIVE_BENCH_NEAR_CLIP;
    }

    if(mode == GL_QUADS) {
        bool all_visible = true;
        for(int i = 0; i < count; i += 4) {
            if(count - i > 4) PREFETCH(in + i + 4);
            all_visible &= _glTrustedFinalPackPair(
                in + i, in + i + 1, output + i, output + i + 1,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX);
            all_visible &= _glTrustedFinalPackPair(
                in + i + 3, in + i + 2, output + i + 2, output + i + 3,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL);
        }
        return all_visible ? GL_KOS_NATIVE_BENCH_OK
                           : GL_KOS_NATIVE_BENCH_NEAR_CLIP;
    }

    bool all_visible = true;
    int i = 0;
    for(; count - i >= 2; i += 2) {
        if(count - i > 2) PREFETCH(in + i + 2);
        const uint32_t fb = (count - i == 2)
            ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX;
        all_visible &= _glTrustedFinalPackPair(
            in + i, in + i + 1, output + i, output + i + 1,
            GPU_CMD_VERTEX, fb);
    }
    if(i < count) {
        all_visible &= _glTrustedFinalPackSingle(
            in + i, output + i, GPU_CMD_VERTEX_EOL);
    }
    return all_visible ? GL_KOS_NATIVE_BENCH_OK
                       : GL_KOS_NATIVE_BENCH_NEAR_CLIP;
}
#endif

/* Production trusted N3 writer. It retains complete-block early near
   rejection so a declined transient batch does not pay a second full
   transform before the exact F1 fallback. Partial output is private
   reservation RAM and is discarded by the caller on rejection. */
int SceneBuildTrustedFinalP3T2BGRA(
        unsigned int mode, const void* vertices, int count, Vertex* output) {
    const GLKosVertexP3T2BGRA* in =
        (const GLKosVertexP3T2BGRA*)vertices;

    if(mode == GL_TRIANGLES) {
        int i = 0;
        for(; count - i >= 6; i += 6) {
            if(count - i > 2) PREFETCH(in + i + 2);
            const bool r0 = _glTrustedFinalPackPair(
                in + i, in + i + 1, output + i, output + i + 1,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX);
            const bool r1 = _glTrustedFinalPackPair(
                in + i + 2, in + i + 3, output + i + 2, output + i + 3,
                GPU_CMD_VERTEX_EOL, GPU_CMD_VERTEX);
            const bool r2 = _glTrustedFinalPackPair(
                in + i + 4, in + i + 5, output + i + 4, output + i + 5,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL);
            if(unlikely(!(r0 & r1 & r2))) return SCENE_FINAL_BUILD_NEAR;
        }
        if(i < count) {
            if(count - i > 2) PREFETCH(in + i + 2);
            const bool r0 = _glTrustedFinalPackPair(
                in + i, in + i + 1, output + i, output + i + 1,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX);
            const bool r1 = _glTrustedFinalPackSingle(
                in + i + 2, output + i + 2, GPU_CMD_VERTEX_EOL);
            if(unlikely(!(r0 & r1))) return SCENE_FINAL_BUILD_NEAR;
        }
        return SCENE_FINAL_BUILD_OK;
    }

    if(mode == GL_QUADS) {
        for(int i = 0; i < count; i += 4) {
            if(count - i > 4) PREFETCH(in + i + 4);
            const bool r0 = _glTrustedFinalPackPair(
                in + i, in + i + 1, output + i, output + i + 1,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX);
            const bool r1 = _glTrustedFinalPackPair(
                in + i + 3, in + i + 2, output + i + 2, output + i + 3,
                GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL);
            if(unlikely(!(r0 & r1))) return SCENE_FINAL_BUILD_NEAR;
        }
        return SCENE_FINAL_BUILD_OK;
    }

    int i = 0;
    for(; count - i >= 2; i += 2) {
        if(count - i > 2) PREFETCH(in + i + 2);
        const uint32_t fb = (count - i == 2)
            ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX;
        if(unlikely(!_glTrustedFinalPackPair(
                in + i, in + i + 1, output + i, output + i + 1,
                GPU_CMD_VERTEX, fb))) {
            return SCENE_FINAL_BUILD_NEAR;
        }
    }
    if(i < count && unlikely(!_glTrustedFinalPackSingle(
            in + i, output + i, GPU_CMD_VERTEX_EOL))) {
        return SCENE_FINAL_BUILD_NEAR;
    }
    return SCENE_FINAL_BUILD_OK;
}

#if defined(GLDC_NATIVE_BENCH) && GLDC_NATIVE_BENCH
int SceneNativeBenchFinalizeClassic(Vertex* vertices, int count) {
    /* Scan before mutation: a rejected near-plane case leaves the classic
       clip-space records intact for the ordinary clipper/fallback. */
    bool all_visible = true;
    for(int i = 0; i < count; ++i) {
        all_visible &= vertices[i].xyz[2] >= -vertices[i].w;
    }
    if(!all_visible) return GL_KOS_NATIVE_BENCH_NEAR_CLIP;
    _glPerspectiveDivideVertex(vertices, count);
    return GL_KOS_NATIVE_BENCH_OK;
}

GL_FORCE_INLINE bool _glNativeBenchPackSQ(
        const GLKosVertexP3T2BGRA* in, uintptr_t d, uint32_t flags,
        float x, float y, float z, float w) {
    const bool visible = _glTrustedFinalFillRecord(
        in, (uint32_t*)d, flags, x, y, z, w);
    __asm__ __volatile__("pref @%0" : : "r"(d) : "memory");
    return visible;
}

GL_FORCE_INLINE bool _glNativeBenchPackPairSQ(
        const GLKosVertexP3T2BGRA* a, const GLKosVertexP3T2BGRA* b,
        uintptr_t da, uintptr_t db, uint32_t fa, uint32_t fb) {
    float axyz[3], bxyz[3], aw, bw;
    TransformVertex2(a->x, a->y, a->z, axyz, &aw,
                     b->x, b->y, b->z, bxyz, &bw);
    const bool av = _glNativeBenchPackSQ(
        a, da, fa, axyz[0], axyz[1], axyz[2], aw);
    const bool bv = _glNativeBenchPackSQ(
        b, db, fb, bxyz[0], bxyz[1], bxyz[2], bw);
    return av && bv;
}

static bool _glNativeBenchSubmitTrianglesSQ(
        const GLKosVertexP3T2BGRA* in, int count, uintptr_t* destination) {
    uintptr_t d = *destination;
    bool all_visible = true;
    int i = 0;
    for(; count - i >= 6; i += 6, d += 6 * 32) {
        if(count - i > 2) PREFETCH(in + i + 2);
        all_visible &= _glNativeBenchPackPairSQ(
            in + i, in + i + 1, d, d + 32,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX);
        all_visible &= _glNativeBenchPackPairSQ(
            in + i + 2, in + i + 3, d + 64, d + 96,
            GPU_CMD_VERTEX_EOL, GPU_CMD_VERTEX);
        all_visible &= _glNativeBenchPackPairSQ(
            in + i + 4, in + i + 5, d + 128, d + 160,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL);
    }
    if(i < count) {
        if(count - i > 2) PREFETCH(in + i + 2);
        all_visible &= _glNativeBenchPackPairSQ(
            in + i, in + i + 1, d, d + 32,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX);
        float xyz[3], w;
        TransformVertex(in[i + 2].x, in[i + 2].y, in[i + 2].z,
                        1.0f, xyz, &w);
        all_visible &= _glNativeBenchPackSQ(
            in + i + 2, d + 64, GPU_CMD_VERTEX_EOL,
            xyz[0], xyz[1], xyz[2], w);
        d += 3 * 32;
    }
    *destination = d;
    return all_visible;
}

static bool _glNativeBenchSubmitStripSQ(
        const GLKosVertexP3T2BGRA* in, int count, uintptr_t* destination) {
    uintptr_t d = *destination;
    bool all_visible = true;
    int i = 0;

    /* Keep EOL out of the long-run loop. The old pair loop tested
       (count - i == 2) on every pair; GCC carried the remaining count on the
       stack and reloaded it for every test. Six fixed non-EOL records mirror
       the proven triangle-soup schedule while always leaving at least two
       records for the bounded exact strip tail below. */
    for(; count - i >= 8; i += 6, d += 6 * 32) {
        PREFETCH(in + i + 2);
        all_visible &= _glNativeBenchPackPairSQ(
            in + i, in + i + 1, d, d + 32,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX);
        PREFETCH(in + i + 4);
        all_visible &= _glNativeBenchPackPairSQ(
            in + i + 2, in + i + 3, d + 64, d + 96,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX);
        PREFETCH(in + i + 6);
        all_visible &= _glNativeBenchPackPairSQ(
            in + i + 4, in + i + 5, d + 128, d + 160,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX);
    }

    /* At most seven records remain. Consume ordinary pairs until the final
       one or two records, so short strips retain their dual-FTRV shape and
       only this bounded tail decides where EOL belongs. */
    for(; count - i > 2; i += 2, d += 64) {
        PREFETCH(in + i + 2);  /* remaining > 2: provably inside the input */
        all_visible &= _glNativeBenchPackPairSQ(
            in + i, in + i + 1, d, d + 32,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX);
    }

    if(count - i == 2) {
        all_visible &= _glNativeBenchPackPairSQ(
            in + i, in + i + 1, d, d + 32,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL);
        d += 64;
    } else {
        assert(count - i == 1);
        float xyz[3], w;
        TransformVertex(in[i].x, in[i].y, in[i].z, 1.0f, xyz, &w);
        all_visible &= _glNativeBenchPackSQ(
            in + i, d, GPU_CMD_VERTEX_EOL, xyz[0], xyz[1], xyz[2], w);
        d += 32;
    }
    *destination = d;
    return all_visible;
}
#endif

static uintptr_t sq_dest_addr = 0;
static bool submit_vertex_fog = false;
static inline bool is_header(const Vertex* v);

/* One final-record sink shared by F1, N2, N3 packets, clipped geometry and
   sprite sidecars. The production build retains the original TA-bound store
   queues exactly. N4 reserves aligned cached RAM in the active KOS list half;
   SceneListFinishChecked publishes the aggregate byte count once. */
GL_FORCE_INLINE uintptr_t _glOutputReserveRecords(size_t count) {
#if GLDC_N4_VERTEX_DMA
    gl_assert(n4_output_open);
    const size_t available = n4_output_open &&
                             n4_output_cursor <= n4_output_limit
        ? (size_t)((n4_output_limit - n4_output_cursor) / sizeof(Vertex))
        : 0u;
    if(unlikely(!n4_output_open || count > available)) {
        fprintf(stderr,
                "GLdc N4: list %d payload overflow (%lu records requested,"
                " %lu available)\n",
                (int)n4_output_list, (unsigned long)count,
                (unsigned long)available);
        _glSubmissionFatal();
    }
    const uintptr_t destination = n4_output_cursor;
    n4_output_cursor += count * sizeof(Vertex);
    return destination;
#else
    (void)count;
    return sq_dest_addr;
#endif
}

GL_FORCE_INLINE void _glOutputCopyRecords(
        uintptr_t destination, const void* source, size_t count) {
#if GLDC_N4_VERTEX_DMA
    memcpy_fast((void*)destination, source, count * sizeof(Vertex));
#else
    sq_fast_cpy((void*)destination, source, count);
#endif
}

GL_FORCE_INLINE void _glOutputAllocateRecord(uintptr_t destination) {
#if GLDC_N4_VERTEX_DMA
    VERTEX_CACHE_ALLOC((void*)destination);
#else
    (void)destination;
#endif
}

GL_FORCE_INLINE void _glOutputCommitRecord(uintptr_t destination) {
#if GLDC_N4_VERTEX_DMA
    (void)destination;
#else
    __asm__ __volatile__("pref @%0" : : "r"(destination) : "memory");
#endif
}

GL_FORCE_INLINE void _glOutputWait(void) {
#if !GLDC_N4_VERTEX_DMA
    sq_wait();
#endif
}

GL_FORCE_INLINE bool _glHeaderUsesVertexFog(const Vertex* v) {
    const uint32_t mode2 = ((const uint32_t*)v)[2];
    return ((mode2 & GPU_TA_PM2_FOG_MASK) >> GPU_TA_PM2_FOG_SHIFT) ==
           GPU_FOG_VERTEX;
}

/* Vertex fog reuses queued color alpha as its interpolated coefficient. Once
   W has been consumed by the divide, move that byte into oargb.a and restore
   the opaque base alpha expected by this lane. PVR combines the textured base
   with the global vertex-fog color; no second polygon or translucent pass is
   involved. Translucent untextured details use MIX_UNTEXTURED instead. */
GL_FORCE_INLINE void _glApplyVertexFog(Vertex* v) {
    const uint8_t fog = v->bgra[3];
    ((uint32_t*)v)[6] = (((uint32_t*)v)[6] & 0x00ffffffu) | 0xff000000u;
    ((uint32_t*)v)[7] = PACK_ARGB8888(fog, 0, 0, 0);
}

static inline void _glPushHeaderOrVertex(Vertex* v, size_t count)  {
    TRACE();
    /* This is the generic clip path's actual TA output count, including
       vertices created by clipping and any headers it submits. */
    GLDC_STAT_ADD(scene_generic_records, (GLuint)count);

#if CLIP_DEBUG
    fprintf(stderr, "{%f, %f, %f, %f}, // %x (%x)\n", v->xyz[0], v->xyz[1], v->xyz[2], v->w, v->flags, v);
#endif

    /* Generic clipping submits headers one record at a time. Decode them
       before choosing the fast copy path because the new header controls all
       following records in the strip. */
    if(count == 1 && is_header(v)) {
        submit_vertex_fog = _glHeaderUsesVertexFog(v);
        const uintptr_t destination = _glOutputReserveRecords(1u);
        _glOutputCopyRecords(destination, v, 1u);
    } else if(likely(!submit_vertex_fog)) {
        const uintptr_t destination = _glOutputReserveRecords(count);
        _glOutputCopyRecords(destination, v, count);
    } else {
        /* Generic clipping submits groups of at most two records. Preserve the
           source: a clipped strip may queue one of these vertices again as the
           next triangle's first point. Convert the whole group in one aligned
           scratch block, then retain one block SQ copy instead of one call per
           record. */
        Vertex __attribute__((aligned(32))) fogged[2];
        assert(count <= 2);
        for(size_t i = 0; i < count; ++i) {
            fogged[i] = v[i];
            if(is_header(&fogged[i])) {
                submit_vertex_fog = _glHeaderUsesVertexFog(&fogged[i]);
            } else if(submit_vertex_fog) {
                _glApplyVertexFog(&fogged[i]);
            }
        }
        const uintptr_t destination = _glOutputReserveRecords(count);
        _glOutputCopyRecords(destination, fogged, count);
    }
}

static inline void _glClipEdge(const Vertex* const v1, const Vertex* const v2, Vertex* vout) {
    const float d0 = v1->w + v1->xyz[2];
    const float d1 = v2->w + v2->xyz[2];

    /* Phase 1: Replace sqrtf(x*x) with fabsf — mathematically identical,
     * saves ~20 SH4 cycles per clip edge. Original was:
     *   t = fabsf(d0) * (1.0f / sqrtf((d1 - d0) * (d1 - d0)))
     * which is just |d0| / |d1 - d0| computed the expensive way. */
    const float denom = d1 - d0;
    float t = fabsf(d0) / fabsf(denom);

    /* Phase 1: Directional epsilon — nudge t toward the inside vertex to
     * prevent rounding from leaving the clipped vertex behind the near plane.
     * Extracted from GLdc better-clipping branch (GL/clip.c line 33). */
#define CLIP_EPSILON 1e-6f
    t += (denom > 0.0f) ? CLIP_EPSILON : -CLIP_EPSILON;

    const float invt = 1.0f - t;

    GLDC_STAT_INC(clip_edges_generated);

    vout->xyz[0] = invt * v1->xyz[0] + t * v2->xyz[0];
    vout->xyz[1] = invt * v1->xyz[1] + t * v2->xyz[1];
    vout->xyz[2] = invt * v1->xyz[2] + t * v2->xyz[2];

    vout->uv[0] = invt * v1->uv[0] + t * v2->uv[0];
    vout->uv[1] = invt * v1->uv[1] + t * v2->uv[1];

    vout->w = invt * v1->w + t * v2->w;

    /* Perspective verts overwrite xyz[2] with 1/|w| at divide time, so this
       floor is inert there; for w==1 ortho verts xyz[2] feeds the ortho depth
       formula and flooring a legitimately negative clipped z collapsed its
       depth to ~1.0 (AUD-001-OPA-14). */
    if(vout->w != 1.0f) {
        vout->xyz[2] = (vout->xyz[2] < FLT_EPSILON) ? FLT_EPSILON : vout->xyz[2];
    }

    vout->bgra[0] = invt * v1->bgra[0] + t * v2->bgra[0];
    vout->bgra[1] = invt * v1->bgra[1] + t * v2->bgra[1];
    vout->bgra[2] = invt * v1->bgra[2] + t * v2->bgra[2];
    vout->bgra[3] = invt * v1->bgra[3] + t * v2->bgra[3];
}

#define SPAN_SORT_CFG 0x005F8030
static volatile uint32_t* PVR_LMMODE0 = (uint32_t*) 0xA05F6884;
static volatile uint32_t *PVR_LMMODE1 = (uint32_t*) 0xA05F6888;

static inline void _glPrepareSubmissionRegisters(void) {
#if GLDC_N4_VERTEX_DMA
    if(n4_submission_registers_initialized) return;
#endif
    PVR_SET(SPAN_SORT_CFG, 0x0);
    *PVR_LMMODE0 = 0;
    *PVR_LMMODE1 = 0;
#if GLDC_N4_VERTEX_DMA
    n4_submission_registers_initialized = true;
#endif
}

#if defined(GLDC_NATIVE_BENCH) && GLDC_NATIVE_BENCH
static bool _glNativeBenchIsOpaquePolyHeader(const void* record) {
    const uint32_t cmd = ((const uint32_t*)record)[0];
    return (cmd & 0xf0800000u) == 0x80800000u &&
           ((cmd & GPU_TA_CMD_TYPE_MASK) >> GPU_TA_CMD_TYPE_SHIFT) ==
               GPU_LIST_OP_POLY;
}

GLint APIENTRY glKosNativeBenchSubmitFinalPacket(
        const GLKosNativeBenchRecord* packet, GLsizei packet_records) {
    if(!packet || packet_records <= 0 ||
       packet_records > NATIVE_BENCH_INPUT_RECORD_LIMIT ||
       ((uintptr_t)packet & 31u) != 0 ||
       !_glNativeBenchIsOpaquePolyHeader(packet)) {
        return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
    }

    /* Exclusive benchmark envelope: pvr_list_begin owns SQ/QACR. Keep the
       same TA setup as SceneListSubmit. One contiguous SQ copy is essential:
       header -> first vertex and every later record then alternate SQ0/SQ1 by
       address bit 5 without restarting at SQ0. */
    pvr_scene_begin();
    if(pvr_list_begin(PVR_LIST_OP_POLY) < 0) {
        pvr_scene_finish();
        return GL_KOS_NATIVE_BENCH_PVR_ERROR;
    }
    _glPrepareSubmissionRegisters();

    sq_fast_cpy((void*)SQ_MASK_DEST(PVR_TA_INPUT), packet,
                (size_t)packet_records);
    _glOutputWait();

    const int list_result = pvr_list_finish();
    const int scene_result = pvr_scene_finish();
    if(list_result < 0 || scene_result < 0) {
        return GL_KOS_NATIVE_BENCH_PVR_ERROR;
    }
    return GL_KOS_NATIVE_BENCH_OK;
}

int SceneNativeBenchSubmitP3T2BGRAAllVisible(
        const void* header, unsigned int mode, const void* vertices,
        const int* counts, int strip_count, int total_count) {
    if(!header || !vertices || ((uintptr_t)header & 31u) != 0 ||
       ((uintptr_t)vertices & 3u) != 0 || total_count < 3 ||
       total_count > NATIVE_BENCH_INPUT_RECORD_LIMIT - 1 ||
       !_glNativeBenchIsOpaquePolyHeader(header) ||
       (mode != GL_TRIANGLES && mode != GL_TRIANGLE_STRIP)) {
        return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
    }
    if(counts && (mode != GL_TRIANGLE_STRIP || strip_count <= 0)) {
        return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
    }

    pvr_scene_begin();
    if(pvr_list_begin(PVR_LIST_OP_POLY) < 0) {
        pvr_scene_finish();
        return GL_KOS_NATIVE_BENCH_PVR_ERROR;
    }
    _glPrepareSubmissionRegisters();
    const uintptr_t ta = (uintptr_t)SQ_MASK_DEST(PVR_TA_INPUT);
    sq_fast_cpy((void*)ta, header, 1);
    /* The header occupied SQ0. Begin vertices at SQ1, then carry this address
       through every strip instead of restarting the queue sequence. */
    uintptr_t destination = ta + sizeof(Vertex);

    const GLKosVertexP3T2BGRA* in =
        (const GLKosVertexP3T2BGRA*)vertices;
    bool all_visible = true;
    if(mode == GL_TRIANGLES) {
        all_visible = _glNativeBenchSubmitTrianglesSQ(
            in, total_count, &destination);
    } else if(!counts) {
        all_visible = _glNativeBenchSubmitStripSQ(
            in, total_count, &destination);
    } else {
        int first = 0;
        for(int s = 0; s < strip_count; ++s) {
            all_visible &= _glNativeBenchSubmitStripSQ(
                in + first, counts[s], &destination);
            first += counts[s];
        }
    }
    _glOutputWait();

    const int list_result = pvr_list_finish();
    const int scene_result = pvr_scene_finish();
    if(list_result < 0 || scene_result < 0) {
        return GL_KOS_NATIVE_BENCH_PVR_ERROR;
    }
    return all_visible ? GL_KOS_NATIVE_BENCH_OK
                       : GL_KOS_NATIVE_BENCH_NEAR_CLIP;
}
#endif

enum Visible {
    NONE_VISIBLE = 0,
    FIRST_VISIBLE = 1,
    SECOND_VISIBLE = 2,
    THIRD_VISIBLE = 4,
    FIRST_AND_SECOND_VISIBLE = FIRST_VISIBLE | SECOND_VISIBLE,
    SECOND_AND_THIRD_VISIBLE = SECOND_VISIBLE | THIRD_VISIBLE,
    FIRST_AND_THIRD_VISIBLE = FIRST_VISIBLE | THIRD_VISIBLE,
    ALL_VISIBLE = 7
};

static inline bool is_header(const Vertex* v) {
    /* Header cmd is 0x80840000; vertex/EOL are 0xE0000000/0xF0000000.
     * Single unsigned compare beats two equality tests. */
    return v->flags < (uint32_t)GPU_CMD_VERTEX;
}

/* The exact per-triangle clipping/output fallback for strips that cross the
   near plane (or are malformed). Visibility classification is reused between
   neighboring triangles; clipping/divide/output arithmetic is unchanged. The fast
   wrapper below feeds it single header-less strip spans, so the minimum
   renderable span here is 3 vertices, not header+3. */
static void SceneListSubmitGeneric(Vertex* vertices, int n, bool vertex_fog) {
    TRACE();

    if(n < 3) {
        return;
    }

    /* Stats and the PVR submission registers (SPAN_SORT_CFG, LMMODE0/1,
       sq_dest_addr) are owned by the finalizer's prologue — the sole caller. */

#if CLIP_DEBUG
    fprintf(stderr, "----\n");

    Vertex* vertex = (Vertex*) vertices;
    for(int i = 0; i < n; ++i) {
        fprintf(stderr, "IN: {%f, %f, %f, %f}, // %x (%x)\n", vertex[i].xyz[0], vertex[i].xyz[1], vertex[i].xyz[2], vertex[i].w, vertex[i].flags, &vertex[i]);
    }
#endif

#if GLDC_SWAP_TELEMETRY
    /* Count in a local across SQ memory barriers, then publish once. A global
       update per clipped output group would perturb the work being measured. */
    GLuint output_records = 0;
#define PUSH_GENERIC_RECORDS(v, count) do { \
    output_records += (GLuint)(count); \
    _glPushHeaderOrVertex((v), (count)); \
} while(0)
#else
#define PUSH_GENERIC_RECORDS(v, count) _glPushHeaderOrVertex((v), (count))
#endif

    submit_vertex_fog = vertex_fog;

    /* This is a bit cumbersome - in some cases (particularly case 2)
       we finish the vertex submission with a duplicated final vertex so
       that the tri-strip can be continued. However, if the next triangle in the
       strip is not visible then the duplicated vertex would've been sent without
       the EOL flag. We won't know if we need the EOL flag or not when processing
       case 2. To workaround this we may queue a vertex temporarily here, in the normal
       case it will be submitted by the next iteration with the same flags it had, but
       in the invisible case it will be overridden to submit with EOL */
    static Vertex __attribute__((aligned(32))) qv;
    Vertex* queued_vertex = NULL;

    /* Use fmov.d-paired copy (fschg) for the 32-byte queue write — this fires
     * every iteration in the ALL_VISIBLE common path, so the savings add up. */
#define QUEUE_VERTEX(v) \
    do { queued_vertex = &qv; memcpy_vertex(queued_vertex, (v)); } while(0)

#define SUBMIT_QUEUED_VERTEX(sflags) \
    do { if(queued_vertex) { queued_vertex->flags = (sflags); PUSH_GENERIC_RECORDS(queued_vertex, 1); queued_vertex = NULL; } } while(0)

    int visible_mask = 0;
    bool first_triangle = true;

    /* Hoisted out of the loop: stable address across iterations (a queued
     * pointer into scratch must remain valid into the next iter's submit),
     * and avoids the per-iter stack adjust. */
    Vertex __attribute__((aligned(32))) scratch[4];

    Vertex* v0 = vertices;
    Vertex* const vend = vertices + n;
    for(int i = 0; i < n - 1; ++i, ++v0) {
        /* Prefetch one line ahead (Vertex is 32 bytes = one SH4 cache line). */
        if(likely(v0 + 2 < vend)) PREFETCH(v0 + 2);

        if(unlikely(is_header(v0))) {
            PUSH_GENERIC_RECORDS(v0, 1);
            visible_mask = 0;
            first_triangle = true;
            GLDC_STAT_INC(scene_headers_seen);
            continue;
        }

        Vertex* v1 = v0 + 1;
        Vertex* v2 = (i < n - 2) ? v0 + 2 : NULL;

        assert(!is_header(v1));

        // We are trailing if we're on the penultimate vertex, or the next but one vertex is
        // an EOL, or v1 is an EOL (FIXME: possibly unnecessary and coverted by the other case?)
        bool is_trailing = (v1->flags == GPU_CMD_VERTEX_EOL) || ((v2) ? is_header(v2) : true);

        if(is_trailing) {
            // OK so we've hit a new context header
            // we need to finalize this strip and move on

            // If the last triangle was all visible, we need
            // to submit the last two vertices, any clipped triangles
            // would've
            if(visible_mask == ALL_VISIBLE) {
                SUBMIT_QUEUED_VERTEX(qv.flags);

                _glPerspectiveDivideVertex(v0, 2);
                v1->flags = GPU_CMD_VERTEX_EOL;
                PUSH_GENERIC_RECORDS(v0, 2);
            } else {
                // If the previous triangle wasn't all visible, and we
                // queued a vertex - we force it to be EOL and submit
                SUBMIT_QUEUED_VERTEX(GPU_CMD_VERTEX_EOL);
            }

            i++;
            v0++;
            visible_mask = 0;
            first_triangle = true;
            continue;
        }

        /* Adjacent strip triangles share two still-unmodified input vertices.
           The cases below divide only v0 in place; v1/v2 use scratch copies.
           Reuse their classifications instead of loading/comparing Z/W three
           times per triangle. Headers and strip tails reset the rolling mask. */
        if(first_triangle) {
            visible_mask = (v0->xyz[2] >= -v0->w) |
                           ((v1->xyz[2] >= -v1->w) << 1);
            first_triangle = false;
        } else {
            visible_mask >>= 1;
        }
        visible_mask |= (v2->xyz[2] >= -v2->w) << 2;

        /* Phase 0: Clipping instrumentation */
        GLDC_STAT_INC(clip_triangles_tested);
        if (visible_mask == ALL_VISIBLE) {
            GLDC_STAT_INC(clip_all_visible);
        } else if (visible_mask == NONE_VISIBLE) {
            GLDC_STAT_INC(clip_none_visible);
        } else {
            GLDC_STAT_INC(clip_partial);
        }

        /* If we've gone behind the plane, we finish the strip
        otherwise we submit however it was */
        if(visible_mask == NONE_VISIBLE) {
            SUBMIT_QUEUED_VERTEX(GPU_CMD_VERTEX_EOL);
        } else {
            SUBMIT_QUEUED_VERTEX(qv.flags);
        }

#if CLIP_DEBUG
        fprintf(stderr, "0x%x 0x%x 0x%x -> %d\n", v0, v1, v2, visible_mask);
#endif

        Vertex* a = &scratch[0], *b = &scratch[1], *c = &scratch[2], *d = &scratch[3];

        if(likely(visible_mask == ALL_VISIBLE)) {
            _glPerspectiveDivideVertex(v0, 1);
            QUEUE_VERTEX(v0);
            continue;
        }

        switch(visible_mask) {
            case ALL_VISIBLE:
                /* unreachable — handled by the fast path above */
                __builtin_unreachable();
            break;
            case NONE_VISIBLE:
                break;
            case FIRST_VISIBLE:
                _glClipEdge(v0, v1, a);
                a->flags = GPU_CMD_VERTEX;

                _glClipEdge(v2, v0, b);
                b->flags = GPU_CMD_VERTEX;

                _glPerspectiveDivideVertex(v0, 1);
                PUSH_GENERIC_RECORDS(v0, 1);

                _glPerspectiveDivideVertex(a, 2);
                PUSH_GENERIC_RECORDS(a, 2);

                QUEUE_VERTEX(b);
            break;
            case SECOND_VISIBLE:
                memcpy_vertex(c, v1);

                _glClipEdge(v0, v1, a);
                a->flags = GPU_CMD_VERTEX;

                _glClipEdge(v1, v2, b);
                b->flags = v2->flags;

                _glPerspectiveDivideVertex(a, 3);
                PUSH_GENERIC_RECORDS(a, 1);

                PUSH_GENERIC_RECORDS(c, 1);

                QUEUE_VERTEX(b);
            break;
            case THIRD_VISIBLE:
                memcpy_vertex(c, v2);

                _glClipEdge(v1, v2, a);
                a->flags = GPU_CMD_VERTEX;

                _glClipEdge(v2, v0, b);
                b->flags = GPU_CMD_VERTEX;

                _glPerspectiveDivideVertex(a, 3);
                PUSH_GENERIC_RECORDS(a, 2);

                QUEUE_VERTEX(c);
            break;
            case FIRST_AND_SECOND_VISIBLE:
                memcpy_vertex(c, v1);

                _glClipEdge(v2, v0, b);
                b->flags = GPU_CMD_VERTEX;

                _glPerspectiveDivideVertex(v0, 1);
                PUSH_GENERIC_RECORDS(v0, 1);

                _glClipEdge(v1, v2, a);
                a->flags = v2->flags;

                _glPerspectiveDivideVertex(a, 3);

                PUSH_GENERIC_RECORDS(c, 1);

                PUSH_GENERIC_RECORDS(b, 2);

                QUEUE_VERTEX(a);
            break;
            case SECOND_AND_THIRD_VISIBLE:
                memcpy_vertex(c, v1);
                memcpy_vertex(d, v2);

                _glClipEdge(v0, v1, a);
                a->flags = GPU_CMD_VERTEX;

                _glClipEdge(v2, v0, b);
                b->flags = GPU_CMD_VERTEX;

                _glPerspectiveDivideVertex(a, 4);
                PUSH_GENERIC_RECORDS(a, 1);

                PUSH_GENERIC_RECORDS(c, 1);

                PUSH_GENERIC_RECORDS(b, 2);

                QUEUE_VERTEX(d);
            break;
            case FIRST_AND_THIRD_VISIBLE:
                memcpy_vertex(c, v2);
                c->flags = GPU_CMD_VERTEX;

                _glClipEdge(v0, v1, a);
                a->flags = GPU_CMD_VERTEX;

                _glClipEdge(v1, v2, b);
                b->flags = GPU_CMD_VERTEX;

                _glPerspectiveDivideVertex(v0, 1);
                PUSH_GENERIC_RECORDS(v0, 1);

                _glPerspectiveDivideVertex(a, 3);
                PUSH_GENERIC_RECORDS(a, 1);

                PUSH_GENERIC_RECORDS(c, 1);

                PUSH_GENERIC_RECORDS(b, 1);

                QUEUE_VERTEX(c);
            break;
            default:
                fprintf(stderr, "ERROR\n");
        }
    }

    SUBMIT_QUEUED_VERTEX(GPU_CMD_VERTEX_EOL);
    submit_vertex_fog = false;

    GLDC_SWAP_WORK_ADD(generic_output_records, output_records);
#undef PUSH_GENERIC_RECORDS
#undef SUBMIT_QUEUED_VERTEX
#undef QUEUE_VERTEX
    _glOutputWait();
}

#ifdef _arch_dreamcast
static inline bool is_internal_segment(const Vertex* v) {
    return v->flags == GLDC_DEFERRED_P3T2BGRA_SENTINEL;
}
#endif

#if defined(GLDC_NATIVE_BENCH) && GLDC_NATIVE_BENCH
typedef struct NativeBenchPacketSink {
    Vertex* packet;
    int capacity;
    int count;
} NativeBenchPacketSink;

/* Count every record even after capacity is exhausted so callers receive the
   exact required size. The successfully written prefix remains contiguous. */
static void _glNativeBenchPacketPush(
        NativeBenchPacketSink* sink, const Vertex* records, int count) {
    int writable = sink->capacity - sink->count;
    if(writable < 0) writable = 0;
    if(writable > count) writable = count;
    if(writable > 0) {
        memcpy(sink->packet + sink->count, records,
               (size_t)writable * sizeof(Vertex));
    }
    sink->count += count;
}

/* Exact specialization of SceneListSubmitGeneric for one independent
   EOL-terminated triangle. The production edge/divide primitives and the
   switch's submission order (including continuation duplicates) are retained,
   while the sink is RAM rather than the TA store queues. */
static void _glNativeBenchClipTriangleToPacket(
        Vertex* v, NativeBenchPacketSink* sink) {
    const int visible_mask =
        ((v[0].xyz[2] >= -v[0].w) << 0) |
        ((v[1].xyz[2] >= -v[1].w) << 1) |
        ((v[2].xyz[2] >= -v[2].w) << 2);
    Vertex __attribute__((aligned(32))) scratch[4];
    Vertex __attribute__((aligned(32))) queued;
    bool have_queued = false;
    Vertex* const a = &scratch[0];
    Vertex* const b = &scratch[1];
    Vertex* const c = &scratch[2];
    Vertex* const d = &scratch[3];

#define BENCH_QUEUE(vertex) \
    do { memcpy_vertex(&queued, (vertex)); have_queued = true; } while(0)
#define BENCH_PUSH(vertex, n) \
    _glNativeBenchPacketPush(sink, (vertex), (n))

    if(visible_mask == ALL_VISIBLE) {
        _glPerspectiveDivideVertex(v, 1);
        BENCH_QUEUE(v);
    } else {
        switch(visible_mask) {
            case NONE_VISIBLE:
                break;
            case FIRST_VISIBLE:
                _glClipEdge(&v[0], &v[1], a);
                a->flags = GPU_CMD_VERTEX;
                _glClipEdge(&v[2], &v[0], b);
                b->flags = GPU_CMD_VERTEX;
                _glPerspectiveDivideVertex(&v[0], 1);
                BENCH_PUSH(&v[0], 1);
                _glPerspectiveDivideVertex(a, 2);
                BENCH_PUSH(a, 2);
                BENCH_QUEUE(b);
                break;
            case SECOND_VISIBLE:
                memcpy_vertex(c, &v[1]);
                _glClipEdge(&v[0], &v[1], a);
                a->flags = GPU_CMD_VERTEX;
                _glClipEdge(&v[1], &v[2], b);
                b->flags = v[2].flags;
                _glPerspectiveDivideVertex(a, 3);
                BENCH_PUSH(a, 1);
                BENCH_PUSH(c, 1);
                BENCH_QUEUE(b);
                break;
            case THIRD_VISIBLE:
                memcpy_vertex(c, &v[2]);
                _glClipEdge(&v[1], &v[2], a);
                a->flags = GPU_CMD_VERTEX;
                _glClipEdge(&v[2], &v[0], b);
                b->flags = GPU_CMD_VERTEX;
                _glPerspectiveDivideVertex(a, 3);
                BENCH_PUSH(a, 2);
                BENCH_QUEUE(c);
                break;
            case FIRST_AND_SECOND_VISIBLE:
                memcpy_vertex(c, &v[1]);
                _glClipEdge(&v[2], &v[0], b);
                b->flags = GPU_CMD_VERTEX;
                _glPerspectiveDivideVertex(&v[0], 1);
                BENCH_PUSH(&v[0], 1);
                _glClipEdge(&v[1], &v[2], a);
                a->flags = v[2].flags;
                _glPerspectiveDivideVertex(a, 3);
                BENCH_PUSH(c, 1);
                BENCH_PUSH(b, 2);
                BENCH_QUEUE(a);
                break;
            case SECOND_AND_THIRD_VISIBLE:
                memcpy_vertex(c, &v[1]);
                memcpy_vertex(d, &v[2]);
                _glClipEdge(&v[0], &v[1], a);
                a->flags = GPU_CMD_VERTEX;
                _glClipEdge(&v[2], &v[0], b);
                b->flags = GPU_CMD_VERTEX;
                _glPerspectiveDivideVertex(a, 4);
                BENCH_PUSH(a, 1);
                BENCH_PUSH(c, 1);
                BENCH_PUSH(b, 2);
                BENCH_QUEUE(d);
                break;
            case FIRST_AND_THIRD_VISIBLE:
                memcpy_vertex(c, &v[2]);
                c->flags = GPU_CMD_VERTEX;
                _glClipEdge(&v[0], &v[1], a);
                a->flags = GPU_CMD_VERTEX;
                _glClipEdge(&v[1], &v[2], b);
                b->flags = GPU_CMD_VERTEX;
                _glPerspectiveDivideVertex(&v[0], 1);
                BENCH_PUSH(&v[0], 1);
                _glPerspectiveDivideVertex(a, 3);
                BENCH_PUSH(a, 1);
                BENCH_PUSH(c, 1);
                BENCH_PUSH(b, 1);
                BENCH_QUEUE(c);
                break;
            default:
                __builtin_unreachable();
        }
    }

    if(have_queued) {
        queued.flags = visible_mask == ALL_VISIBLE
            ? GPU_CMD_VERTEX : GPU_CMD_VERTEX_EOL;
        BENCH_PUSH(&queued, 1);
    }
    if(visible_mask == ALL_VISIBLE) {
        _glPerspectiveDivideVertex(&v[1], 2);
        v[2].flags = GPU_CMD_VERTEX_EOL;
        BENCH_PUSH(&v[1], 2);
    }

#undef BENCH_PUSH
#undef BENCH_QUEUE
}

int SceneNativeBenchBuildTrianglePacketP3T2BGRA(
        const void* header, const void* vertices, int count,
        Vertex* packet, int packet_capacity, int* packet_records) {
    if(!header || !vertices || !packet || !packet_records || count < 3 ||
       count % 3 != 0 || packet_capacity < 1 ||
       count > ((INT_MAX - 1) / 5) * 3) {
        return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
    }

    memcpy(packet, header, sizeof(Vertex));
    NativeBenchPacketSink sink = {
        .packet = packet,
        .capacity = packet_capacity,
        .count = 1
    };
    const GLKosVertexP3T2BGRA* in =
        (const GLKosVertexP3T2BGRA*)vertices;

    for(int first = 0; first < count; first += 3) {
        Vertex __attribute__((aligned(32))) triangle[3];
        VERTEX_CACHE_ALLOC(&triangle[0]);
        VERTEX_CACHE_ALLOC(&triangle[1]);
        VERTEX_CACHE_ALLOC(&triangle[2]);
        TransformVertex2(
            in[first].x, in[first].y, in[first].z,
            triangle[0].xyz, &triangle[0].w,
            in[first + 1].x, in[first + 1].y, in[first + 1].z,
            triangle[1].xyz, &triangle[1].w);
        TransformVertex(
            in[first + 2].x, in[first + 2].y, in[first + 2].z, 1.0f,
            triangle[2].xyz, &triangle[2].w);
        for(int i = 0; i < 3; ++i) {
            triangle[i].flags = i == 2
                ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX;
            triangle[i].uv[0] = in[first + i].u;
            triangle[i].uv[1] = in[first + i].v;
            *((uint32_t*)triangle[i].bgra) = in[first + i].bgra;
        }
        _glNativeBenchClipTriangleToPacket(triangle, &sink);
    }

    *packet_records = sink.count;
    return sink.count <= sink.capacity
        ? GL_KOS_NATIVE_BENCH_OK : GL_KOS_NATIVE_BENCH_CAPACITY;
}
#endif

/* Fused divide+submit for an all-visible run (2026-07-16, the heavy-city gap):
   the three-pass shape (scan, divide-in-place, sq_fast_cpy) reads the run's
   bytes three times, and at heavy city load the op+tr walks are the largest
   byte volume in the frame. This replaces divide+copy: read each 32-byte
   record ONCE, perspective-divide in registers, store straight to the store
   queues. Same contract sq_fast_cpy relies on: QACR is already TA-bound by
   SceneListBegin — pvr_list_begin calls sq_lock(PVR_TA_INPUT), which programs
   QACR (pvr_dr_init is a deprecated no-op in current KOS; 2026-07-16 audit
   correction) — and successive +32 destinations alternate SQ0/SQ1 by address
   bit 5. Headers ride the run verbatim. The math is the
   _glPerspectiveDivideVertex math verbatim (same fsrra, same w==1 ortho
   branch), so the TA sees byte-identical records. */
static void _glDivideSubmitRun(Vertex* v, int n, bool initial_vertex_fog) {
    /* One input record becomes exactly one TA record on this all-visible
       path, so the run length is also its actual output count. */
    GLDC_STAT_ADD(scene_divided_records, (GLuint)n);
    GLDC_SWAP_WORK_ADD(ordinary_divided_records, n);
    uintptr_t d = _glOutputReserveRecords((size_t)n);
    bool vertex_fog = initial_vertex_fog;
    for(; n--; ++v, d += 32) {
        uint32_t* q = (uint32_t*)d;
        _glOutputAllocateRecord(d);
        PREFETCH(v + 2);
        if(unlikely(is_header(v))) {
            const uint32_t* s = (const uint32_t*)v;
            vertex_fog = ((s[2] & GPU_TA_PM2_FOG_MASK) >>
                          GPU_TA_PM2_FOG_SHIFT) == GPU_FOG_VERTEX;
            q[0] = s[0]; q[1] = s[1]; q[2] = s[2]; q[3] = s[3];
            q[4] = s[4]; q[5] = s[5]; q[6] = s[6]; q[7] = s[7];
        } else {
            const float w = v->w;
            const float f = _glFastInvert(w);
            float z;
            if(unlikely(w == 1.0f)) {
                z = _glFastInvert(1.0001f + v->xyz[2]);
            } else {
                z = f;
            }
            q[0] = v->flags;
            ((float*)q)[1] = v->xyz[0] * f;
            ((float*)q)[2] = v->xyz[1] * f;
            ((float*)q)[3] = z;
            q[4] = ((const uint32_t*)v)[4];
            q[5] = ((const uint32_t*)v)[5];
            if(unlikely(vertex_fog)) {
                const uint32_t base = ((const uint32_t*)v)[6];
                q[6] = (base & 0x00ffffffu) | 0xff000000u;
                q[7] = (uint32_t)v->bgra[3] << 24;
            } else {
                q[6] = ((const uint32_t*)v)[6];
                q[7] = ((const uint32_t*)v)[7];
            }
        }
        _glOutputCommitRecord(d);   /* fire SQ, or retain cached N4 RAM */
    }
}

#ifdef _arch_dreamcast
/* Keep the classify-ahead window inside the SH4's data cache. The input side
   is 24 bytes/record, so 128 records occupy 3 KiB and are still hot when the
   direct writer immediately consumes the accepted run. */
#define GLDC_DEFERRED_CLASSIFY_RECORDS 128

GL_FORCE_INLINE bool _glDeferredQuadAllVisible(
        const GLdcDeferredP3T2BGRA* descriptor,
        const GLKosVertexP3T2BGRA* in) {
    const float* const m = descriptor->mvp;
    const float offset_inv = descriptor->polygon_offset_inv;

    for(int i = 0; i < 4; ++i) {
        const float x = in[i].x;
        const float y = in[i].y;
        const float z0 = in[i].z;
        const float z = x * m[2] + y * m[6] + z0 * m[10] + m[14];
        const float w = x * m[3] + y * m[7] + z0 * m[11] + m[15];

        /* _glBakePolygonOffset scales W only when transformed W != 1. Require
           BOTH possible near tests. That is conservative for the exact-W==1
           exception and for either sign of offset; an ambiguous record takes
           the exact generic path instead of being speculatively SQ'd. Scalar
           dot order is not bit-identical to FTRV, so retain a cancellation-
           scaled tolerance around the plane as generic too. */
        const float near_plain = z + w;
        const float near_offset = z + w * offset_inv;
        const float scale = __builtin_fabsf(z) + __builtin_fabsf(w) *
                            (1.0f + __builtin_fabsf(offset_inv)) + 1.0f;
        const float margin = 32.0f * FLT_EPSILON * scale;
        /* Negated comparisons also route unordered/non-finite results to the
           exact path on toolchains that preserve IEEE comparison semantics. */
        if(!(near_plain > margin) || !(near_offset > margin)) return false;
    }
    return true;
}

GL_FORCE_INLINE void _glDeferredFillRecordSQ(
        const GLKosVertexP3T2BGRA* in, uintptr_t destination,
        uint32_t flags, float x, float y, float z, float w,
        float offset_inv) {
    /* Reproduce draw.c's offset bake before the same reciprocal sequence. In
       particular word 7 is the baked W, not the object-space or unscaled W. */
    if(unlikely(w != 1.0f && offset_inv != 1.0f)) {
        x *= offset_inv;
        y *= offset_inv;
        w *= offset_inv;
    }

    const float f = _glFastInvert(w);
    uint32_t* const q = (uint32_t*)destination;
    _glOutputAllocateRecord(destination);
    q[0] = flags;
    ((float*)q)[1] = x * f;
    ((float*)q)[2] = y * f;
    ((float*)q)[3] = unlikely(w == 1.0f)
        ? _glFastInvert(1.0001f + z) : f;
    ((float*)q)[4] = in->u;
    ((float*)q)[5] = in->v;
    q[6] = in->bgra;
    ((float*)q)[7] = w;
    _glOutputCommitRecord(destination);
}

GL_FORCE_INLINE void _glDeferredPackPairSQ(
        const GLKosVertexP3T2BGRA* a,
        const GLKosVertexP3T2BGRA* b,
        uintptr_t da, uintptr_t db, uint32_t fa, uint32_t fb,
        float offset_inv) {
    float axyz[3], bxyz[3], aw, bw;
    TransformVertex2(a->x, a->y, a->z, axyz, &aw,
                     b->x, b->y, b->z, bxyz, &bw);
    _glDeferredFillRecordSQ(a, da, fa,
                            axyz[0], axyz[1], axyz[2], aw, offset_inv);
    _glDeferredFillRecordSQ(b, db, fb,
                            bxyz[0], bxyz[1], bxyz[2], bw, offset_inv);
}

GL_FORCE_INLINE void _glDeferredPackSingleSQ(
        const GLKosVertexP3T2BGRA* in, uintptr_t destination,
        uint32_t flags, float offset_inv) {
    float xyz[3], w;
    TransformVertex(in->x, in->y, in->z, 1.0f, xyz, &w);
    _glDeferredFillRecordSQ(in, destination, flags,
                            xyz[0], xyz[1], xyz[2], w, offset_inv);
}

static void _glDeferredSubmitVisibleQuads(
        const GLdcDeferredP3T2BGRA* descriptor,
        const GLKosVertexP3T2BGRA* in, int count) {
    uintptr_t d = _glOutputReserveRecords((size_t)count);
    const float offset_inv = descriptor->polygon_offset_inv;

    GLDC_STAT_ADD(scene_divided_records, (GLuint)count);
    GLDC_STAT_ADD(deferred_direct_vertices, (GLuint)count);
    GLDC_SWAP_WORK_ADD(deferred_direct_vertices, count);
    for(int i = 0; i < count; i += 4, d += 4 * 32) {
        PREFETCH(in + i + 2);
        _glDeferredPackPairSQ(in + i, in + i + 1, d, d + 32,
                              GPU_CMD_VERTEX, GPU_CMD_VERTEX, offset_inv);
        if(i + 4 < count) PREFETCH(in + i + 4);
        _glDeferredPackPairSQ(in + i + 3, in + i + 2, d + 64, d + 96,
                              GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL, offset_inv);
    }
}

static void _glDeferredSubmitNearQuad(
        const GLdcDeferredP3T2BGRA* descriptor,
        const GLKosVertexP3T2BGRA* in, bool vertex_fog) {
    Vertex __attribute__((aligned(32))) quad[4];
    VERTEX_CACHE_ALLOC(&quad[0]);
    VERTEX_CACHE_ALLOC(&quad[1]);
    VERTEX_CACHE_ALLOC(&quad[2]);
    VERTEX_CACHE_ALLOC(&quad[3]);

    TransformVertex2(in[0].x, in[0].y, in[0].z, quad[0].xyz, &quad[0].w,
                     in[1].x, in[1].y, in[1].z, quad[1].xyz, &quad[1].w);
    TransformVertex2(in[3].x, in[3].y, in[3].z, quad[2].xyz, &quad[2].w,
                     in[2].x, in[2].y, in[2].z, quad[3].xyz, &quad[3].w);

    static const uint8_t source_index[4] = {0, 1, 3, 2};
    for(int i = 0; i < 4; ++i) {
        const GLKosVertexP3T2BGRA* const source = in + source_index[i];
        quad[i].flags = i == 3 ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX;
        quad[i].uv[0] = source->u;
        quad[i].uv[1] = source->v;
        *((uint32_t*)quad[i].bgra) = source->bgra;
        if(quad[i].w != 1.0f && descriptor->polygon_offset_inv != 1.0f) {
            quad[i].xyz[0] *= descriptor->polygon_offset_inv;
            quad[i].xyz[1] *= descriptor->polygon_offset_inv;
            quad[i].w *= descriptor->polygon_offset_inv;
        }
    }

    GLDC_STAT_INC(deferred_near_quads);
    GLDC_SWAP_WORK_ADD(deferred_near_quads, 1u);
    SceneListSubmitGeneric(quad, 4, vertex_fog);
}

static void _glSubmitDeferredInterleavedP3T2BGRA(
        const GLdcDeferredP3T2BGRA* descriptor) {
    const GLKosVertexP3T2BGRA* const vertices =
        descriptor->input.interleaved;
    const int count = (int)descriptor->count;
    int run_first = 0;
    int run_count = 0;

    for(int first = 0; first < count; first += 4) {
        const bool all_visible =
            _glDeferredQuadAllVisible(descriptor, vertices + first);
        if(all_visible) {
            if(run_count == 0) run_first = first;
            run_count += 4;
            if(run_count < GLDC_DEFERRED_CLASSIFY_RECORDS &&
               first + 4 < count) {
                continue;
            }
        }

        if(run_count > 0) {
            _glDeferredSubmitVisibleQuads(
                descriptor, vertices + run_first, run_count);
            run_count = 0;
        }
        if(!all_visible) {
            _glDeferredSubmitNearQuad(
                descriptor, vertices + first, false);
        }
    }
}

/* Independent triangles use the N1 fixed-six schedule: two complete
   triangles per hot-loop iteration, with EOL baked into records 2 and 5.
   Because count is divisible by three, the only possible tail is one exact
   three-record triangle. The try-call has already proven every vertex clear
   of both near planes, so this path never stages or invokes the clipper. */
static void _glSubmitDeferredTrianglesP3T2BGRA(
        const GLdcDeferredP3T2BGRA* descriptor) {
    const GLKosVertexP3T2BGRA* const in =
        descriptor->input.interleaved;
    const int count = (int)descriptor->count;
    uintptr_t d = _glOutputReserveRecords((size_t)count);
    const float offset_inv = descriptor->polygon_offset_inv;
    int i = 0;

    gl_assert(in && count >= 3 && count % 3 == 0);
    GLDC_STAT_INC(deferred_triangle_descriptors_submitted);
    GLDC_STAT_ADD(scene_divided_records, (GLuint)count);
    GLDC_STAT_ADD(deferred_direct_vertices, (GLuint)count);
    GLDC_SWAP_WORK_ADD(deferred_direct_vertices, count);
    GLDC_STAT_ADD(deferred_triangle_direct_vertices, (GLuint)count);

    for(; count - i >= 6; i += 6, d += 6 * 32) {
        PREFETCH(in + i + 2);
        _glDeferredPackPairSQ(
            in + i, in + i + 1, d, d + 32,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX, offset_inv);
        PREFETCH(in + i + 4);
        _glDeferredPackPairSQ(
            in + i + 2, in + i + 3, d + 64, d + 96,
            GPU_CMD_VERTEX_EOL, GPU_CMD_VERTEX, offset_inv);
        _glDeferredPackPairSQ(
            in + i + 4, in + i + 5, d + 128, d + 160,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL, offset_inv);
    }

    if(i < count) {
        gl_assert(count - i == 3);
        PREFETCH(in + i + 2);
        _glDeferredPackPairSQ(
            in + i, in + i + 1, d, d + 32,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX, offset_inv);
        _glDeferredPackSingleSQ(
            in + i + 2, d + 64, GPU_CMD_VERTEX_EOL, offset_inv);
    }
}

/* Drain one strip whose complete referenced payload was conservatively proven
   all-visible by the side-effect-free try-call. This is the hardware-tested
   N1 fixed-six schedule adapted to the production descriptor's snapshotted
   polygon offset. EOL is decided only in the bounded tail. */
static void _glDeferredSubmitVisibleStrip(
        const GLdcDeferredP3T2BGRA* descriptor,
        const GLKosVertexP3T2BGRA* in, int count,
        uintptr_t* destination) {
    uintptr_t d = *destination;
    const float offset_inv = descriptor->polygon_offset_inv;
    int i = 0;

    GLDC_STAT_ADD(scene_divided_records, (GLuint)count);
    GLDC_STAT_ADD(deferred_direct_vertices, (GLuint)count);
    GLDC_SWAP_WORK_ADD(deferred_direct_vertices, count);
    GLDC_STAT_ADD(deferred_multistrip_direct_vertices, (GLuint)count);

    for(; count - i >= 8; i += 6, d += 6 * 32) {
        PREFETCH(in + i + 2);
        _glDeferredPackPairSQ(
            in + i, in + i + 1, d, d + 32,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX, offset_inv);
        PREFETCH(in + i + 4);
        _glDeferredPackPairSQ(
            in + i + 2, in + i + 3, d + 64, d + 96,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX, offset_inv);
        PREFETCH(in + i + 6);
        _glDeferredPackPairSQ(
            in + i + 4, in + i + 5, d + 128, d + 160,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX, offset_inv);
    }

    for(; count - i > 2; i += 2, d += 64) {
        PREFETCH(in + i + 2);
        _glDeferredPackPairSQ(
            in + i, in + i + 1, d, d + 32,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX, offset_inv);
    }

    if(count - i == 2) {
        _glDeferredPackPairSQ(
            in + i, in + i + 1, d, d + 32,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL, offset_inv);
        d += 64;
    } else {
        gl_assert(count - i == 1);
        _glDeferredPackSingleSQ(
            in + i, d, GPU_CMD_VERTEX_EOL, offset_inv);
        d += 32;
    }
    *destination = d;
}

static void _glSubmitDeferredMultiStripsP3T2BGRA(
        const GLdcDeferredP3T2BGRA* descriptor) {
    const GLKosVertexP3T2BGRA* const vertices =
        descriptor->input.interleaved;
    const GLKosStripRange* const strips = descriptor->strips;
    uintptr_t destination =
        _glOutputReserveRecords((size_t)descriptor->count);

    gl_assert(vertices && strips && descriptor->strip_count > 0);
    GLDC_STAT_INC(deferred_multistrip_descriptors_submitted);
    for(GLuint s = 0; s < descriptor->strip_count; ++s) {
        gl_assert(strips[s].count >= 3u && strips[s].count <= INT_MAX);
        _glDeferredSubmitVisibleStrip(
            descriptor, vertices + strips[s].first,
            (int)strips[s].count, &destination);
    }
}

GL_FORCE_INLINE bool _glDeferredArrayQuadAllVisible(
        const GLdcDeferredP3T2BGRA* descriptor, int first) {
    const float* const m = descriptor->mvp;
    const float offset_inv = descriptor->polygon_offset_inv;
    const float* p = descriptor->input.arrays.positions + first * 3;

    for(int i = 0; i < 4; ++i, p += 3) {
        const float x = p[0];
        const float y = p[1];
        const float z0 = p[2];
        const float z = x * m[2] + y * m[6] + z0 * m[10] + m[14];
        const float w = x * m[3] + y * m[7] + z0 * m[11] + m[15];
        const float near_plain = z + w;
        const float near_offset = z + w * offset_inv;
        const float scale = __builtin_fabsf(z) + __builtin_fabsf(w) *
                            (1.0f + __builtin_fabsf(offset_inv)) + 1.0f;
        const float margin = 32.0f * FLT_EPSILON * scale;
        if(!(near_plain > margin) || !(near_offset > margin)) return false;
    }
    return true;
}

#if GLDC_N2_BATCH_CLASSIFY
/* Return only complete visible quads from the existing bounded classify-ahead
   window. Keeping this scalar scan out of the SQ writer's large dispatcher
   hoists matrix/offset setup across the run without a transformed-vertex cache.
   It must not touch XMTRX: the following writer uses the already loaded matrix.
   The first failing vertex ends the scan; its whole quad takes the old clipper. */
static GL_NO_INLINE int _glDeferredArrayVisiblePrefix(
        const GLdcDeferredP3T2BGRA* descriptor, int first, int count) {
    const float* const m = descriptor->mvp;
    const float offset_inv = descriptor->polygon_offset_inv;
    const float* p = descriptor->input.arrays.positions + first * 3;

    /* At identity offset both original near tests are identical. Dispatch
       once per window, retaining the original scalar Z/W and margin order. */
    if(offset_inv == 1.0f) {
        for(int i = 0; i < count; ++i, p += 3) {
            const float x = p[0];
            const float y = p[1];
            const float z0 = p[2];
            const float z = x * m[2] + y * m[6] + z0 * m[10] + m[14];
            const float w = x * m[3] + y * m[7] + z0 * m[11] + m[15];
            const float near_plain = z + w;
            const float scale = __builtin_fabsf(z) + __builtin_fabsf(w) *
                                (1.0f + __builtin_fabsf(1.0f)) + 1.0f;
            const float margin = 32.0f * FLT_EPSILON * scale;
            if(!(near_plain > margin)) return i & ~3;
        }
        return count;
    }

    for(int i = 0; i < count; ++i, p += 3) {
        const float x = p[0];
        const float y = p[1];
        const float z0 = p[2];
        const float z = x * m[2] + y * m[6] + z0 * m[10] + m[14];
        const float w = x * m[3] + y * m[7] + z0 * m[11] + m[15];
        const float near_plain = z + w;
        const float near_offset = z + w * offset_inv;
        const float scale = __builtin_fabsf(z) + __builtin_fabsf(w) *
                            (1.0f + __builtin_fabsf(offset_inv)) + 1.0f;
        const float margin = 32.0f * FLT_EPSILON * scale;
        if(!(near_plain > margin) || !(near_offset > margin)) return i & ~3;
    }
    return count;
}
#endif

GL_FORCE_INLINE void _glDeferredWriteArrayRecordSQ(
        const float* uv, uint32_t bgra, uintptr_t destination,
        uint32_t flags, float x, float y, float z, float w,
        float f, bool vertex_fog) {
    uint32_t* const q = (uint32_t*)destination;
    _glOutputAllocateRecord(destination);
    q[0] = flags;
    ((float*)q)[1] = x * f;
    ((float*)q)[2] = y * f;
    ((float*)q)[3] = unlikely(w == 1.0f)
        ? _glFastInvert(1.0001f + z) : f;
    ((float*)q)[4] = uv[0];
    ((float*)q)[5] = uv[1];
    if(unlikely(vertex_fog)) {
        q[6] = (bgra & 0x00ffffffu) | 0xff000000u;
        q[7] = bgra & 0xff000000u;
    } else {
        q[6] = bgra;
        ((float*)q)[7] = w;
    }
    _glOutputCommitRecord(destination);
}

GL_FORCE_INLINE void _glDeferredFillArrayRecordSQ(
        const float* uv, uint32_t bgra, uintptr_t destination,
        uint32_t flags, float x, float y, float z, float w,
        float offset_inv, bool vertex_fog) {
    if(unlikely(w != 1.0f && offset_inv != 1.0f)) {
        x *= offset_inv;
        y *= offset_inv;
        w *= offset_inv;
    }

    const float f = _glFastInvert(w);
    _glDeferredWriteArrayRecordSQ(uv, bgra, destination, flags,
                                    x, y, z, w, f, vertex_fog);
}

GL_FORCE_INLINE void _glDeferredPackArrayPairSQ(
        const float* pa, const float* pb,
        const float* ua, const float* ub,
        uint32_t ca, uint32_t cb,
        uintptr_t da, uintptr_t db, uint32_t fa, uint32_t fb,
        float offset_inv, bool vertex_fog) {
    float axyz[3], bxyz[3], aw, bw;
    TransformVertex2(pa[0], pa[1], pa[2], axyz, &aw,
                     pb[0], pb[1], pb[2], bxyz, &bw);
#if GLDC_N2_ARRAY_PAIR_PREP
    /* Offset policy is shared by the pair; keep each original W==1
       exception when it is active. Prepare both independent reciprocals
       before the first SQ commit so their latency can overlap. Every
       vertex retains the original multiply/FSRRA/depth operation order. */
    if(unlikely(offset_inv != 1.0f)) {
        if(aw != 1.0f) {
            axyz[0] *= offset_inv; axyz[1] *= offset_inv; aw *= offset_inv;
        }
        if(bw != 1.0f) {
            bxyz[0] *= offset_inv; bxyz[1] *= offset_inv; bw *= offset_inv;
        }
    }
    const float af = _glFastInvert(aw);
    const float bf = _glFastInvert(bw);
    _glDeferredWriteArrayRecordSQ(ua, ca, da, fa,
                                  axyz[0], axyz[1], axyz[2], aw, af, vertex_fog);
    _glDeferredWriteArrayRecordSQ(ub, cb, db, fb,
                                  bxyz[0], bxyz[1], bxyz[2], bw, bf, vertex_fog);
#else
    _glDeferredFillArrayRecordSQ(ua, ca, da, fa,
        axyz[0], axyz[1], axyz[2], aw, offset_inv, vertex_fog);
    _glDeferredFillArrayRecordSQ(ub, cb, db, fb,
        bxyz[0], bxyz[1], bxyz[2], bw, offset_inv, vertex_fog);
#endif
}

static void _glDeferredSubmitVisibleArrayQuads(
        const GLdcDeferredP3T2BGRA* descriptor,
        int first, int count, bool vertex_fog) {
    const float* p = descriptor->input.arrays.positions + first * 3;
    const float* u = descriptor->input.arrays.texcoords + first * 2;
    const uint32_t* c = (const uint32_t*)(
        descriptor->input.arrays.colors + first * 4);
    uintptr_t d = _glOutputReserveRecords((size_t)count);
    const float offset_inv = descriptor->polygon_offset_inv;
    /* The descriptor is immutable throughout this drain. Keep these fields
       in locals: SQ commit's memory barrier otherwise forces descriptor
       reloads on each quad even though the hardware cannot modify them. */
    const GLboolean constant_color = descriptor->constant_color;
    const GLuint constant_bgra = descriptor->constant_bgra;

    GLDC_STAT_ADD(scene_divided_records, (GLuint)count);
    GLDC_STAT_ADD(deferred_direct_vertices, (GLuint)count);
    GLDC_SWAP_WORK_ADD(deferred_direct_vertices, count);
    GLDC_STAT_ADD(deferred_array_direct_vertices, (GLuint)count);
    if(constant_color)
        GLDC_STAT_ADD(deferred_color_array_direct_vertices, (GLuint)count);
    for(int i = 0; i < count;
        i += 4, p += 12, u += 8, c += 4, d += 4 * 32) {
        /* Classify-ahead has already warmed positions. Prime the next quad's
           three source lines without reading beyond the borrowed extents on
           the final quad. */
        if(i + 4 < count) {
            PREFETCH(p + 12);
            PREFETCH(u + 8);
            if(!constant_color) PREFETCH(c + 4);
        }
        const uint32_t c0 = constant_color ? constant_bgra : c[0];
        const uint32_t c1 = constant_color ? constant_bgra : c[1];
        const uint32_t c2 = constant_color ? constant_bgra : c[2];
        const uint32_t c3 = constant_color ? constant_bgra : c[3];
        _glDeferredPackArrayPairSQ(
            p, p + 3, u, u + 2, c0, c1, d, d + 32,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX, offset_inv, vertex_fog);
        _glDeferredPackArrayPairSQ(
            p + 9, p + 6, u + 6, u + 4, c3, c2, d + 64, d + 96,
            GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL, offset_inv, vertex_fog);
    }
}

/* Same final records as the ordinary array lane, but a planar quad's fourth
   homogeneous corner is exactly A+C-B. Footprints are immutable through the
   drain, so deriving D here removes one SH-4 transform per quad without moving
   work back into the frame's submission phase. */
static void _glDeferredSubmitVisiblePlanarArrayQuads(
        const GLdcDeferredP3T2BGRA* descriptor,
        int first, int count, bool vertex_fog) {
    const float* p = descriptor->input.arrays.positions + first * 3;
    const float* u = descriptor->input.arrays.texcoords + first * 2;
    const uint32_t* c = (const uint32_t*)(
        descriptor->input.arrays.colors + first * 4);
    uintptr_t d = _glOutputReserveRecords((size_t)count);
    const float offset_inv = descriptor->polygon_offset_inv;

    GLDC_STAT_ADD(scene_divided_records, (GLuint)count);
    GLDC_STAT_ADD(deferred_direct_vertices, (GLuint)count);
    GLDC_SWAP_WORK_ADD(deferred_direct_vertices, count);
    GLDC_STAT_ADD(deferred_array_direct_vertices, (GLuint)count);
    for(int i = 0; i < count;
        i += 4, p += 12, u += 8, c += 4, d += 4 * 32) {
        if(i + 4 < count) {
            PREFETCH(p + 12);
            PREFETCH(u + 8);
            PREFETCH(c + 4);
        }
        float axyz[3], bxyz[3], cxyz[3], aw, bw, cw;
        TransformVertex2(p[0], p[1], p[2], axyz, &aw,
                         p[3], p[4], p[5], bxyz, &bw);
        TransformVertex(p[6], p[7], p[8], 1.0f, cxyz, &cw);
        const float dxyz[3] = {
            axyz[0] + cxyz[0] - bxyz[0],
            axyz[1] + cxyz[1] - bxyz[1],
            axyz[2] + cxyz[2] - bxyz[2]
        };
        const float dw = aw + cw - bw;

        _glDeferredFillArrayRecordSQ(
            u, c[0], d, GPU_CMD_VERTEX,
            axyz[0], axyz[1], axyz[2], aw, offset_inv, vertex_fog);
        _glDeferredFillArrayRecordSQ(
            u + 2, c[1], d + 32, GPU_CMD_VERTEX,
            bxyz[0], bxyz[1], bxyz[2], bw, offset_inv, vertex_fog);
        _glDeferredFillArrayRecordSQ(
            u + 6, c[3], d + 64, GPU_CMD_VERTEX,
            dxyz[0], dxyz[1], dxyz[2], dw, offset_inv, vertex_fog);
        _glDeferredFillArrayRecordSQ(
            u + 4, c[2], d + 96, GPU_CMD_VERTEX_EOL,
            cxyz[0], cxyz[1], cxyz[2], cw, offset_inv, vertex_fog);
    }
}

static void _glDeferredSubmitNearArrayQuad(
        const GLdcDeferredP3T2BGRA* descriptor,
        int first, bool vertex_fog) {
    const float* const p = descriptor->input.arrays.positions + first * 3;
    const float* const u = descriptor->input.arrays.texcoords + first * 2;
    const uint32_t* const c = (const uint32_t*)(
        descriptor->input.arrays.colors + first * 4);
    Vertex __attribute__((aligned(32))) quad[4];
    VERTEX_CACHE_ALLOC(&quad[0]);
    VERTEX_CACHE_ALLOC(&quad[1]);
    VERTEX_CACHE_ALLOC(&quad[2]);
    VERTEX_CACHE_ALLOC(&quad[3]);

    TransformVertex2(p[0], p[1], p[2], quad[0].xyz, &quad[0].w,
                     p[3], p[4], p[5], quad[1].xyz, &quad[1].w);
    TransformVertex2(p[9], p[10], p[11], quad[2].xyz, &quad[2].w,
                     p[6], p[7], p[8], quad[3].xyz, &quad[3].w);

    static const uint8_t source_index[4] = {0, 1, 3, 2};
    for(int i = 0; i < 4; ++i) {
        const int source = source_index[i];
        quad[i].flags = i == 3 ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX;
        quad[i].uv[0] = u[source * 2];
        quad[i].uv[1] = u[source * 2 + 1];
        *((uint32_t*)quad[i].bgra) = descriptor->constant_color
            ? descriptor->constant_bgra : c[source];
        if(quad[i].w != 1.0f && descriptor->polygon_offset_inv != 1.0f) {
            quad[i].xyz[0] *= descriptor->polygon_offset_inv;
            quad[i].xyz[1] *= descriptor->polygon_offset_inv;
            quad[i].w *= descriptor->polygon_offset_inv;
        }
    }

    GLDC_STAT_INC(deferred_near_quads);
    GLDC_SWAP_WORK_ADD(deferred_near_quads, 1u);
    GLDC_STAT_INC(deferred_array_near_quads);
    if(descriptor->constant_color)
        GLDC_STAT_INC(deferred_color_array_near_quads);
    SceneListSubmitGeneric(quad, 4, vertex_fog);
}

static void _glSubmitDeferredArrayP3T2BGRA(
        const GLdcDeferredP3T2BGRA* descriptor, bool vertex_fog) {
    const int count = (int)descriptor->count;
#if GLDC_N2_BATCH_CLASSIFY
    for(int first = 0; first < count;) {
        int limit = count - first;
        if(limit > GLDC_DEFERRED_CLASSIFY_RECORDS)
            limit = GLDC_DEFERRED_CLASSIFY_RECORDS;
        const int visible =
            _glDeferredArrayVisiblePrefix(descriptor, first, limit);
        if(visible > 0) {
            _glDeferredSubmitVisibleArrayQuads(
                descriptor, first, visible, vertex_fog);
            first += visible;
        }
        if(visible < limit) {
            _glDeferredSubmitNearArrayQuad(descriptor, first, vertex_fog);
            first += 4;
        }
    }
#else
    int run_first = 0;
    int run_count = 0;

    for(int first = 0; first < count; first += 4) {
        const bool all_visible =
            _glDeferredArrayQuadAllVisible(descriptor, first);
        if(all_visible) {
            if(run_count == 0) run_first = first;
            run_count += 4;
            if(run_count < GLDC_DEFERRED_CLASSIFY_RECORDS &&
               first + 4 < count) {
                continue;
            }
        }

        if(run_count > 0) {
            _glDeferredSubmitVisibleArrayQuads(
                descriptor, run_first, run_count, vertex_fog);
            run_count = 0;
        }
        if(!all_visible) {
            _glDeferredSubmitNearArrayQuad(
                descriptor, first, vertex_fog);
        }
    }
#endif
}

static void _glSubmitDeferredPlanarArrayP3T2BGRA(
        const GLdcDeferredP3T2BGRA* descriptor, bool vertex_fog) {
    const int count = (int)descriptor->count;
#if GLDC_N2_BATCH_CLASSIFY
    for(int first = 0; first < count;) {
        int limit = count - first;
        if(limit > GLDC_DEFERRED_CLASSIFY_RECORDS)
            limit = GLDC_DEFERRED_CLASSIFY_RECORDS;
        const int visible =
            _glDeferredArrayVisiblePrefix(descriptor, first, limit);
        if(visible > 0) {
            _glDeferredSubmitVisiblePlanarArrayQuads(
                descriptor, first, visible, vertex_fog);
            first += visible;
        }
        if(visible < limit) {
            /* Near planar quads retain all four original transforms. */
            GLDC_STAT_INC(vertices_transformed);
            _glDeferredSubmitNearArrayQuad(descriptor, first, vertex_fog);
            first += 4;
        }
    }
#else
    int run_first = 0;
    int run_count = 0;

    for(int first = 0; first < count; first += 4) {
        const bool all_visible =
            _glDeferredArrayQuadAllVisible(descriptor, first);
        if(all_visible) {
            if(run_count == 0) run_first = first;
            run_count += 4;
            if(run_count < GLDC_DEFERRED_CLASSIFY_RECORDS &&
               first + 4 < count) {
                continue;
            }
        }

        if(run_count > 0) {
            _glDeferredSubmitVisiblePlanarArrayQuads(
                descriptor, run_first, run_count, vertex_fog);
            run_count = 0;
        }
        if(!all_visible) {
            /* The enqueue-side accounting charged three transforms per
               planar quad. Near geometry deliberately takes the ordinary
               four-corner clip path, so account for its one extra transform
               here without pessimizing the common visible run. */
            GLDC_STAT_INC(vertices_transformed);
            _glDeferredSubmitNearArrayQuad(
                descriptor, first, vertex_fog);
        }
    }
#endif
}

static void _glSubmitDeferredP3T2BGRA(
        const GLdcDeferredP3T2BGRA* descriptor, bool vertex_fog) {
    UploadMatrix4x4(&descriptor->mvp);
    GLDC_STAT_INC(deferred_descriptors_submitted);
    if(descriptor->primitive == GLDC_DEFERRED_P3T2BGRA_TRIANGLES) {
        gl_assert(!descriptor->arrays && !vertex_fog);
        _glSubmitDeferredTrianglesP3T2BGRA(descriptor);
    } else if(descriptor->primitive == GLDC_DEFERRED_P3T2BGRA_MULTISTRIPS) {
        gl_assert(!descriptor->arrays && !vertex_fog);
        _glSubmitDeferredMultiStripsP3T2BGRA(descriptor);
    } else if(descriptor->primitive ==
              GLDC_DEFERRED_P3T2BGRA_PLANAR_QUADS) {
        gl_assert(descriptor->arrays);
        GLDC_STAT_INC(deferred_array_descriptors_submitted);
        _glSubmitDeferredPlanarArrayP3T2BGRA(descriptor, vertex_fog);
    } else if(descriptor->arrays) {
        GLDC_STAT_INC(deferred_array_descriptors_submitted);
        if(descriptor->constant_color) {
            gl_assert(!vertex_fog);
            GLDC_STAT_INC(deferred_color_array_descriptors_submitted);
        }
        _glSubmitDeferredArrayP3T2BGRA(descriptor, vertex_fog);
    } else {
        gl_assert(!vertex_fog);
        _glSubmitDeferredInterleavedP3T2BGRA(descriptor);
    }
}

/* Segment decoding is deliberately out of line.  Ordinary F1 records only
   pay the one unlikely marker comparison in SceneListSubmit; the descriptor
   validation/copy machinery does not inflate that strip scanner's hot body. */
static GL_NO_INLINE void _glSubmitPvrPacketSegment(
        const uint32_t* words, bool* vertex_fog) {
    const GLuint descriptor_index = words[1];
    const GLdcPvrPacket* const packet = _glPvrPacketAt(descriptor_index);
    const GLKosPvrRecord* const records = _glPvrPacketRecords(packet);
    const bool sentinel_valid = packet && records &&
        words[2] == ~descriptor_index && words[3] == packet->token &&
        words[7] == (GLDC_PVR_PACKET_SENTINEL ^
                     descriptor_index ^ packet->token) &&
        packet->record_count >= (packet->has_header ? 4u : 3u);
    gl_assert(sentinel_valid);
    if(!sentinel_valid) return;

    /* Header + already-final records are contiguous/aligned;
       pvr_list_begin has already armed QACR for TA input. */
    const uintptr_t destination =
        _glOutputReserveRecords((size_t)packet->record_count);
    _glOutputCopyRecords(destination, records, (size_t)packet->record_count);
    GLDC_STAT_ADD(scene_records_in, packet->record_count - 1u);
    if(packet->has_header) {
        GLDC_STAT_INC(scene_headers_seen);
        *vertex_fog = _glHeaderUsesVertexFog((const Vertex*)records);
    }
    GLDC_STAT_INC(pvr_packet_segments_submitted);
    GLDC_STAT_ADD(pvr_packet_records_submitted, packet->record_count);
}

static GL_NO_INLINE void _glSubmitDeferredSegment(
        const uint32_t* words, bool vertex_fog) {
    const GLuint descriptor_index = words[1];
    const GLdcDeferredP3T2BGRA* const descriptor =
        _glDeferredP3T2BGRAAt(descriptor_index);
    const bool sentinel_valid = descriptor &&
        words[2] == ~descriptor_index &&
        words[7] == (GLDC_DEFERRED_P3T2BGRA_SENTINEL ^ descriptor_index);
    gl_assert(sentinel_valid);
    if(!sentinel_valid) return;

    /* Replace the physical marker counted by the prologue with the
       object-space records it represents. */
    GLDC_STAT_ADD(scene_records_in, descriptor->count - 1u);
    _glSubmitDeferredP3T2BGRA(descriptor, vertex_fog);
}

#if GLDC_N4_VERTEX_DMA
/* Saturating helpers keep a corrupt descriptor from wrapping a small DMA
   allocation. The eventual SIZE_MAX request stops explicitly in the buffer
   sizing guard instead of allowing a record write outside the list half. */
static size_t _glN4BudgetAdd(size_t total, size_t amount) {
    return amount > SIZE_MAX - total ? SIZE_MAX : total + amount;
}

static size_t _glN4BudgetMultiply(size_t count, size_t multiplier) {
    return count > SIZE_MAX / multiplier ? SIZE_MAX : count * multiplier;
}

/* One-plane generic clipping can emit at most five records per source record:
   each input triangle produces no more than a clipped quad plus its queued
   continuation. That intentionally loose bound is used only for the rare
   strip/quad that fails the same conservative near-plane classifier used by
   the submitter. Fully-visible spans retain their exact one-for-one count, so
   ordinary city capacity tracks real traffic rather than reserving 5x RAM. */
size_t SceneListRecordBudget(
        const Vertex* vertices, int n, int sprite_records) {
    size_t budget = sprite_records > 0 ? (size_t)sprite_records : 0u;
    if(!vertices || n <= 0) return budget;

    const Vertex* v = vertices;
    const Vertex* const end = vertices + n;
    while(v < end) {
        if(unlikely(is_internal_segment(v))) {
            const uint32_t* const words = (const uint32_t*)v;
            const GLuint descriptor_index = words[1];
            if(words[3] != 0u) {
                const GLdcPvrPacket* const packet =
                    _glPvrPacketAt(descriptor_index);
                const GLKosPvrRecord* const records =
                    _glPvrPacketRecords(packet);
                const bool valid = packet && records &&
                    words[2] == ~descriptor_index &&
                    words[3] == packet->token &&
                    words[7] == (GLDC_PVR_PACKET_SENTINEL ^
                                 descriptor_index ^ packet->token);
                gl_assert(valid);
                if(!valid) return SIZE_MAX;
                budget = _glN4BudgetAdd(
                    budget, (size_t)packet->record_count);
            } else {
                const GLdcDeferredP3T2BGRA* const descriptor =
                    _glDeferredP3T2BGRAAt(descriptor_index);
                const bool valid = descriptor &&
                    words[2] == ~descriptor_index &&
                    words[7] == (GLDC_DEFERRED_P3T2BGRA_SENTINEL ^
                                 descriptor_index);
                gl_assert(valid);
                if(!valid) return SIZE_MAX;

                if(descriptor->primitive != GLDC_DEFERRED_P3T2BGRA_QUADS &&
                   descriptor->primitive !=
                       GLDC_DEFERRED_P3T2BGRA_PLANAR_QUADS) {
                    budget = _glN4BudgetAdd(
                        budget, (size_t)descriptor->count);
                } else {
                    for(GLuint first = 0; first < descriptor->count;
                        first += 4u) {
                        const bool visible = descriptor->arrays
                            ? _glDeferredArrayQuadAllVisible(
                                  descriptor, (int)first)
                            : _glDeferredQuadAllVisible(
                                  descriptor,
                                  descriptor->input.interleaved + first);
                        budget = _glN4BudgetAdd(
                            budget, visible ? 4u : 20u);
                    }
                }
            }
            ++v;
            continue;
        }

        if(is_header(v)) {
            budget = _glN4BudgetAdd(budget, 1u);
            ++v;
            continue;
        }

        const Vertex* const first = v;
        bool all_visible = true;
        bool terminated = false;
        while(v < end && !is_header(v) && !is_internal_segment(v)) {
            all_visible &= v->xyz[2] >= -v->w;
            ++v;
            if(v[-1].flags == GPU_CMD_VERTEX_EOL) {
                terminated = true;
                break;
            }
        }

        const size_t count = (size_t)(v - first);
        budget = _glN4BudgetAdd(
            budget, all_visible && terminated
                ? count : _glN4BudgetMultiply(count, 5u));
    }

    return budget;
}
#endif
#endif

/* Keep the scan -> fused divide handoff inside the SH4's 16 KiB data cache.
   128 records are 4 KiB, leaving room for the submitter's code/stack and the
   rest of GLdc's hot state. Only cut after a complete strip. */
#define GLDC_VISIBLE_RUN_CACHE_RECORDS 128

/* ---- All-visible run finalizer (2026-07-15, HyperSolar investigation) ----
   [GLDC-T] proved the old submission loop IS the swap cost (wait=0.01ms, op+tr
   ~6.5ms at heavy city load): every vertex paid a 32-byte qv staging copy plus
   a count=1 sq_fast_cpy (FSCHG entry/exit per record, always queue SQ0). But
   near-plane clipping is RARE — almost every strip in a frame is fully
   visible. So: scan the list at STRIP granularity; accumulate maximal
   all-visible runs (headers ride along untouched — they are plain 32-byte
   records); submit each run with _glDivideSubmitRun above, which fuses the
   perspective divide INTO the store-queue write (one read of the run instead
   of the old divide-in-place + copy two). Any strip that crosses the near
   plane (or lacks an EOL terminator) flushes the run and takes
   SceneListSubmitGeneric above — the byte-exact old path. */
void SceneListSubmit(Vertex* vertices, int n) {
    TRACE();

    if(n <= 0) return;
#ifdef _arch_dreamcast
    if(n < 4) {
        /* Descriptor tables are frame-global but this call owns one list. A
           sentinel queued elsewhere must not relax this short stream. */
        GLboolean has_local_segment = GL_FALSE;
        for(int i = 0; i < n; ++i) {
            if(is_internal_segment(vertices + i)) {
                has_local_segment = GL_TRUE;
                break;
            }
        }
        if(!has_local_segment) return;
    }
#else
    if(n < 4) return;
#endif

    GLDC_STAT_INC(scene_list_submits);
    GLDC_STAT_ADD(scene_records_in, n);

    _glPrepareSubmissionRegisters();

    sq_dest_addr = (uintptr_t)SQ_MASK_DEST(PVR_TA_INPUT);

    Vertex* v = vertices;
    Vertex* const vend = vertices + n;
    Vertex* run_start = v;
    bool vertex_fog = false;
    bool run_start_fog = false;

    while(v < vend) {
#ifdef _arch_dreamcast
        if(unlikely(is_internal_segment(v))) {
            if(v > run_start) {
                _glDivideSubmitRun(run_start, (int)(v - run_start),
                                   run_start_fog);
            }

            const uint32_t* const words = (const uint32_t*)v;
            if(words[3] != 0u) {
                _glSubmitPvrPacketSegment(words, &vertex_fog);
            } else {
                _glSubmitDeferredSegment(words, vertex_fog);
            }

            ++v;
            run_start = v;
            run_start_fog = vertex_fog;
            continue;
        }
#endif
        if(is_header(v)) {
            GLDC_STAT_INC(scene_headers_seen);
            vertex_fog = _glHeaderUsesVertexFog(v);
            ++v;                      /* headers ride the current run */
            continue;
        }

        /* Scan this strip: [v .. first EOL]. Track visibility as we go. */
        Vertex* strip_end = v;        /* will point AT the EOL vertex */
        int all_visible = 1;
        while(strip_end < vend && !is_header(strip_end)) {
            PREFETCH(strip_end + 2);
            all_visible &= (strip_end->xyz[2] >= -strip_end->w);
            if(strip_end->flags == GPU_CMD_VERTEX_EOL) break;
            ++strip_end;
        }

        if(strip_end >= vend || is_header(strip_end)) {
            /* Unterminated strip: not renderable as a run — the generic path
               handles (and EOL-terminates) it exactly like the old loop did. */
            all_visible = 0;
            --strip_end;              /* last real vertex of the span */
        }

        GLDC_SWAP_WORK_ADD(ordinary_scan_vertices, strip_end - v + 1);

        if(all_visible) {
            /* Divide is FUSED into the run flush (_glDivideSubmitRun) — the
               strip stays in clip space until submitted. */
            v = strip_end + 1;        /* strip stays in the run */
            if(v - run_start >= GLDC_VISIBLE_RUN_CACHE_RECORDS) {
                _glDivideSubmitRun(run_start, (int)(v - run_start),
                                   run_start_fog);
                run_start = v;
                run_start_fog = vertex_fog;
            }
        } else {
            /* Flush everything accumulated before this strip, then let the
               exact old path do the clip work on the strip alone. */
            if(v > run_start) {
                _glDivideSubmitRun(run_start, (int)(v - run_start),
                                   run_start_fog);
            }
            SceneListSubmitGeneric(v, (int)(strip_end - v) + 1,
                                   vertex_fog);
            v = strip_end + 1;
            run_start = v;
            run_start_fog = vertex_fog;
        }
    }

    if(v > run_start) {
        _glDivideSubmitRun(run_start, (int)(v - run_start),
                           run_start_fog);
    }

    _glOutputWait();
}

/* ---- TA sprite quads (2026-07-16, the glow lane) ----
   Builds compiled sprite headers + 64-byte sprite records into the active
   list's sprite sidecar. The context comes from the SAME state builder the
   poly headers use (_glBuildPolyContext) mapped onto KOS's sprite context —
   GLdc's GPU_* constants are value-identical to the legacy PVR_* ones, so the
   fields transfer raw (EXCEPT filter: GLdc pre-encodes it — see the >>1 below).
   The divide math is the vertex path's verbatim (fsrra,
   w==1 ortho branch) so sprites depth-match coplanar geometry exactly. */
/* Pack a float UV pair into the sprite record's 16-bit-halves format. */
static inline uint32_t _glSpriteUV16(float u, float v) {
    union { float f; uint32_t i; } a, b;
    a.f = u; b.f = v;
    return (a.i & 0xFFFF0000u) | (b.i >> 16);
}

/* The sprite color lives in header words 4/5. Spell out this fixed 32-byte
   copy so hot sprite runs do not pay an out-of-line memcpy call per color
   change (and so the compiler can schedule the eight aligned stores). */
static inline void _glWriteSpriteHeader(uint32_t* dst,
                                        const pvr_sprite_hdr_t* header,
                                        uint32_t argb) {
    const uint32_t* src = (const uint32_t*) header;
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
    dst[4] = argb;
    dst[5] = 0;
    dst[6] = src[6];
    dst[7] = src[7];
}

/* Context compilation is batch setup, not per-sprite work. The specialized
   camera-plane lane keeps this mapping out of its hot transform loop. */
static __attribute__((noinline))
void _glCompileCurrentSpriteHeader(PolyList* out, pvr_sprite_hdr_t* header) {
    PolyContext ctx;
    _glBuildPolyContext(&ctx, out, 0);

    pvr_sprite_cxt_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.list_type        = (pvr_list_t)ctx.list_type;
    sc.gen.alpha        = ctx.gen.alpha;
    sc.gen.fog_type     = ctx.gen.fog_type;
    sc.gen.culling      = ctx.gen.culling;
    sc.gen.color_clamp  = ctx.gen.color_clamp;
    sc.gen.clip_mode    = ctx.gen.clip_mode;
    sc.gen.specular     = ctx.gen.specular;
    sc.blend.src        = ctx.blend.src;
    sc.blend.dst        = ctx.blend.dst;
    sc.blend.src_enable = ctx.blend.src_enable;
    sc.blend.dst_enable = ctx.blend.dst_enable;
    sc.depth.comparison = ctx.depth.comparison;
    sc.depth.write      = ctx.depth.write;
    sc.txr.enable       = ctx.txr.enable;
    sc.txr.filter       = ctx.txr.filter >> 1;
    sc.txr.mipmap       = ctx.txr.mipmap;
    sc.txr.mipmap_bias  = ctx.txr.mipmap_bias;
    sc.txr.uv_flip      = ctx.txr.uv_flip;
    sc.txr.uv_clamp     = ctx.txr.uv_clamp;
    sc.txr.alpha        = ctx.txr.alpha;
    sc.txr.env          = ctx.txr.env;
    sc.txr.width        = ctx.txr.width;
    sc.txr.height       = ctx.txr.height;
    sc.txr.format       = ctx.txr.format;
    sc.txr.base         = (pvr_ptr_t) ctx.txr.base;
    pvr_sprite_compile(header, &sc);
}

/* Total time/growth accounting is shared by every direct TA-sprite lane. */
#if GLDC_SWAP_TELEMETRY
uint32_t _glSpriteCallUs = 0, _glSpriteGrowCount = 0;
uint32_t _glSpriteHdrCount = 0, _glSpriteRecCount = 0;
#endif

void SceneSpriteQuads(const float* pos, const uint32_t* colors, int quads) {
#if GLDC_SWAP_TELEMETRY
    const uint64_t spr_t0 = timer_us_gettime64();
#endif
    PolyList* out = _glActivePolyList();
#if GLDC_S3_SEGMENTED_OP
    if(out != _glOpaquePolyList()) _glS3DrainOP();   /* leaving OP: hot-drain it */
#endif
    AlignedVector* sv = &out->sprites;

    PolyContext ctx;
    _glBuildPolyContext(&ctx, out, 0);

    pvr_sprite_cxt_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.list_type       = ctx.list_type;
    sc.gen.alpha       = ctx.gen.alpha;
    sc.gen.fog_type    = ctx.gen.fog_type;
    sc.gen.culling     = ctx.gen.culling;
    sc.gen.color_clamp = ctx.gen.color_clamp;
    sc.gen.clip_mode   = ctx.gen.clip_mode;
    sc.gen.specular    = ctx.gen.specular;
    sc.blend.src        = ctx.blend.src;
    sc.blend.dst        = ctx.blend.dst;
    sc.blend.src_enable = ctx.blend.src_enable;
    sc.blend.dst_enable = ctx.blend.dst_enable;
    sc.depth.comparison = ctx.depth.comparison;
    sc.depth.write      = ctx.depth.write;
    sc.txr.enable      = ctx.txr.enable;
    /* GLdc pre-encodes GPUFilter as the raw TSP values (0/2/4/6); KOS's sprite
       compiler FIELD_PREPs LOGICAL PVR_FILTER_* (0/1/2/3). Raw copy read
       bilinear(2) as trilinear-pass-1, which point-samples on a non-mipped
       texture (2026-07-16 audit) — the sprite glow shipped nearest-filtered.
       >>1 restores the vertex path's true filtering. */
    sc.txr.filter      = ctx.txr.filter >> 1;
    sc.txr.mipmap      = ctx.txr.mipmap;
    sc.txr.mipmap_bias = ctx.txr.mipmap_bias;
    sc.txr.uv_flip     = ctx.txr.uv_flip;
    sc.txr.uv_clamp    = ctx.txr.uv_clamp;
    sc.txr.alpha       = ctx.txr.alpha;
    sc.txr.env         = ctx.txr.env;
    sc.txr.width       = ctx.txr.width;
    sc.txr.height      = ctx.txr.height;
    sc.txr.format      = ctx.txr.format;
    sc.txr.base        = (pvr_ptr_t) ctx.txr.base;

    pvr_sprite_hdr_t shdr;
    pvr_sprite_compile(&shdr, &sc);

    /* GL polygon-offset parity with SceneSpriteCenters: the finalizer that
       normally applies it never sees these records (AUD-001-OPA-09). */
    const float depth_mul = _glPolygonOffsetMul;

    uint32_t last_argb = 0;
    int have_hdr = 0;
    const uint32_t base_blocks = aligned_vector_size(sv);
#if GLDC_SWAP_TELEMETRY
    const uint32_t cap_before = aligned_vector_capacity(sv);
#endif
    uint32_t* const batch = (uint32_t*) aligned_vector_extend(
        sv, (uint32_t)quads * 3u);  /* worst case: header + 64-byte sprite */
#if GLDC_SWAP_TELEMETRY
    if(aligned_vector_capacity(sv) != cap_before) _glSpriteGrowCount++;
#endif
    uint32_t used_blocks = 0;
#if GLDC_SWAP_TELEMETRY
    uint32_t hdrs = 0;
#endif

    for(int q = 0; q < quads; q += 2) {
        const int n = (q + 1 < quads) ? 2 : 1;
        const float* p0 = pos + q * 12;
        float xyz[2][4][3];
        float w[2][4];

        /* A sprite input is a planar parallelogram, so clip-space D is
           exactly A+C-B. Across two quads, transform A/B/C as three dual-FTRV
           pairs instead of transforming all four corners. */
        TransformVertex2(p0[0], p0[1], p0[2], xyz[0][0], &w[0][0],
                         p0[3], p0[4], p0[5], xyz[0][1], &w[0][1]);
        if(n == 2) {
            const float* p1 = p0 + 12;
            TransformVertex2(p0[6], p0[7], p0[8], xyz[0][2], &w[0][2],
                             p1[0], p1[1], p1[2], xyz[1][0], &w[1][0]);
            TransformVertex2(p1[3], p1[4], p1[5], xyz[1][1], &w[1][1],
                             p1[6], p1[7], p1[8], xyz[1][2], &w[1][2]);
        } else {
            TransformVertex(p0[6], p0[7], p0[8], 1.0f, xyz[0][2], &w[0][2]);
        }

        for(int k = 0; k < n; ++k) {
            for(int a = 0; a < 3; ++a)
                xyz[k][3][a] = xyz[k][0][a] + xyz[k][2][a] - xyz[k][1][a];
            w[k][3] = w[k][0] + w[k][2] - w[k][1];

        /* No sprite clip path: drop the quad whole if any corner crosses the
           near plane (at that point a glow face is at the screen edge). */
        if(xyz[k][0][2] < -w[k][0] || xyz[k][1][2] < -w[k][1] ||
           xyz[k][2][2] < -w[k][2] || xyz[k][3][2] < -w[k][3]) {
            continue;
        }

        /* Glow scratch layout: 4 equal per-vertex color words per face. BGRA
           bytes in memory read as an ARGB32 word — exactly what the sprite
           header wants. */
        uint32_t argb = colors[(q + k) * 4];
        if(!have_hdr || argb != last_argb) {
            uint32_t* h = batch + used_blocks * 8u;
            VERTEX_CACHE_ALLOC(h);
            _glWriteSpriteHeader(h, &shdr, argb);
            used_blocks++;
#if GLDC_SWAP_TELEMETRY
            hdrs++;
#endif
            last_argb = argb;
            have_hdr = 1;
        }

        pvr_sprite_txr_t* s =
            (pvr_sprite_txr_t*) (batch + used_blocks * 8u);
        used_blocks += 2;
        VERTEX_CACHE_ALLOC(s);
        VERTEX_CACHE_ALLOC((uint8_t*)s + 32);
        const float fa = _glFastInvert(w[k][0]);
        const float fb = _glFastInvert(w[k][1]);
        const float fc = _glFastInvert(w[k][2]);
        const float fd = _glFastInvert(w[k][3]);
        s->flags = GPU_CMD_VERTEX_EOL;
        s->ax = xyz[k][0][0] * fa;
        s->ay = xyz[k][0][1] * fa;
        s->az = ((w[k][0] == 1.0f) ? _glFastInvert(1.0001f + xyz[k][0][2]) : fa) * depth_mul;
        s->bx = xyz[k][1][0] * fb;
        s->by = xyz[k][1][1] * fb;
        s->bz = ((w[k][1] == 1.0f) ? _glFastInvert(1.0001f + xyz[k][1][2]) : fb) * depth_mul;
        s->cx = xyz[k][2][0] * fc;
        s->cy = xyz[k][2][1] * fc;
        s->cz = ((w[k][2] == 1.0f) ? _glFastInvert(1.0001f + xyz[k][2][2]) : fc) * depth_mul;
        s->dx = xyz[k][3][0] * fd;
        s->dy = xyz[k][3][1] * fd;
        s->dummy = 0;
        /* full-texture corner UVs; the fourth is hardware-derived like its Z */
        s->auv = 0x00000000;
        s->buv = 0x3F800000;
        s->cuv = 0x3F803F80;
        }
    }
    aligned_vector_resize(sv, base_blocks + used_blocks);
#if GLDC_SWAP_TELEMETRY
    _glSpriteHdrCount += hdrs;
    _glSpriteRecCount += (used_blocks - hdrs) >> 1;
    _glSpriteCallUs += (uint32_t)(timer_us_gettime64() - spr_t0);
#endif
}

/* Homogeneous sprite family. The two object-space half axes are common to the
   whole call, so transform them once with w=0; every sprite then needs only
   one center FTRV. This is the road/roof glow lane. */
/* Lamp-budget dissection (Bruno 2026-08-04): total us spent INSIDE the two
   sprite-lane builders (pools use SceneSpriteCenters, glare/traffic/signals
   use SceneSpriteCentersPlane) and mid-frame sprite-lane capacity growths
   (each growth memcpys the whole accumulated lane — the multi-ms submit
   spike suspect). Read + reset through glKosTakeSwapTelemetry(). The delta
   between the game's submit brackets and this number is bind/state preamble
   outside the sprite path. */
void SceneSpriteCenters(const float* centers, const uint32_t* colors,
                        const float* half_sizes, const float* uv_rects, int sprites,
                        float ux, float uy, float uz, float vx, float vy, float vz) {
#if GLDC_SWAP_TELEMETRY
    const uint64_t spr_t0 = timer_us_gettime64();
#endif
    PolyList* out = _glActivePolyList();
    AlignedVector* sv = &out->sprites;

    pvr_sprite_hdr_t shdr;
    _glCompileCurrentSpriteHeader(out, &shdr);
    /* The ordinary vertex finalizer applies polygon offset to PVR inverse-W,
       but this direct TA-sprite lane bypasses that finalizer.  Preserve the
       same GL state contract for coplanar sprite decals (HyperSolar's light
       pools) without moving their world-space geometry. */
    const float depth_mul = _glPolygonOffsetMul;

    float tu[3], tv[3], uw, vw;
    TransformVertex(ux, uy, uz, 0.0f, tu, &uw);
    TransformVertex(vx, vy, vz, 0.0f, tv, &vw);
    const float near_u_zw = tu[2] + uw;
    const float near_v_zw = tv[2] + vw;
    /* half_sizes are non-negative by contract. Hoist the absolute transformed
       camera-plane extent once: |u*h|+|v*h| == (|u|+|v|)*h. Dense city-light
       batches now pay one multiply per sprite instead of two multiplies, two
       fabs calls and an add. */
    const float near_extent_scale =
        fabsf(near_u_zw) + fabsf(near_v_zw);

    uint32_t last_argb = 0;
    int have_hdr = 0;
    const uint32_t base_blocks = aligned_vector_size(sv);
#if GLDC_SWAP_TELEMETRY
    const uint32_t cap_before = aligned_vector_capacity(sv);
#endif
    uint32_t* const batch = (uint32_t*) aligned_vector_extend(
        sv, (uint32_t)sprites * 3u);  /* worst case: header + 64-byte sprite */
#if GLDC_SWAP_TELEMETRY
    if(aligned_vector_capacity(sv) != cap_before) _glSpriteGrowCount++;
#endif
    uint32_t used_blocks = 0;
#if GLDC_SWAP_TELEMETRY
    uint32_t hdrs = 0;
#endif
    for(int q = 0; q < sprites; q += 2) {
        const int n = (q + 1 < sprites) ? 2 : 1;
        float tc[2][3], cw[2];
        const float* p = centers + q * 3;
        if(n == 2) {
            TransformVertex2(p[0], p[1], p[2], tc[0], &cw[0],
                             p[3], p[4], p[5], tc[1], &cw[1]);
        } else {
            TransformVertex(p[0], p[1], p[2], 1.0f, tc[0], &cw[0]);
        }

        for(int k = 0; k < n; ++k) {
            const float hs = half_sizes ? half_sizes[q + k] : 1.0f;
            const float sux = tu[0] * hs, suy = tu[1] * hs;
            const float suz = tu[2] * hs, suw = uw * hs;
            const float svx = tv[0] * hs, svy = tv[1] * hs;
            const float svz = tv[2] * hs, svw = vw * hs;
            /* min over all four (clip-Z + W) corners, algebraically exact for
               the parallelogram and cheaper than materializing xyz[4]/w[4]. */
            const float near_extent = near_extent_scale * hs;
            if(tc[k][2] + cw[k] < near_extent)
                continue;

            const float wa = cw[k] - suw - svw;
            const float wb = cw[k] + suw - svw;
            const float wc = cw[k] + suw + svw;
            const float wd = cw[k] - suw + svw;

            uint32_t argb = colors[q + k];
            if(!have_hdr || argb != last_argb) {
                uint32_t* h = batch + used_blocks * 8u;
                VERTEX_CACHE_ALLOC(h);
                _glWriteSpriteHeader(h, &shdr, argb);
                used_blocks++;
#if GLDC_SWAP_TELEMETRY
            hdrs++;
#endif
                last_argb = argb;
                have_hdr = 1;
            }

            pvr_sprite_txr_t* s =
                (pvr_sprite_txr_t*) (batch + used_blocks * 8u);
            used_blocks += 2;
            VERTEX_CACHE_ALLOC(s);
            VERTEX_CACHE_ALLOC((uint8_t*)s + 32);
            const float fa = _glFastInvert(wa);
            const float fb = _glFastInvert(wb);
            const float fc = _glFastInvert(wc);
            const float fd = _glFastInvert(wd);
            s->flags = GPU_CMD_VERTEX_EOL;
            s->ax = (tc[k][0] - sux - svx) * fa;
            s->ay = (tc[k][1] - suy - svy) * fa;
            s->az = ((wa == 1.0f)
                ? _glFastInvert(1.0001f + tc[k][2] - suz - svz) : fa) * depth_mul;
            s->bx = (tc[k][0] + sux - svx) * fb;
            s->by = (tc[k][1] + suy - svy) * fb;
            s->bz = ((wb == 1.0f)
                ? _glFastInvert(1.0001f + tc[k][2] + suz - svz) : fb) * depth_mul;
            s->cx = (tc[k][0] + sux + svx) * fc;
            s->cy = (tc[k][1] + suy + svy) * fc;
            s->cz = ((wc == 1.0f)
                ? _glFastInvert(1.0001f + tc[k][2] + suz + svz) : fc) * depth_mul;
            s->dx = (tc[k][0] - sux + svx) * fd;
            s->dy = (tc[k][1] - suy + svy) * fd;
            s->dummy = 0;
            if(uv_rects) {
                const float* r = uv_rects + (q + k) * 4;
                s->auv = _glSpriteUV16(r[0], r[1]);
                s->buv = _glSpriteUV16(r[2], r[1]);
                s->cuv = _glSpriteUV16(r[2], r[3]);
            } else {
                s->auv = 0x00000000;
                s->buv = 0x3F800000;
                s->cuv = 0x3F803F80;
            }
        }
    }
    aligned_vector_resize(sv, base_blocks + used_blocks);
#if GLDC_SWAP_TELEMETRY
    _glSpriteHdrCount += hdrs;
    _glSpriteRecCount += (used_blocks - hdrs) >> 1;
    _glSpriteCallUs += (uint32_t)(timer_us_gettime64() - spr_t0);
#endif
}

/* Camera-plane center lane. A view-plane billboard has equal homogeneous W
   at all four corners; exploit that exact caller-owned invariant instead of
   constructing four xyz/W vectors and running four FSRRA estimates. The
   transformed axis Z/W extents remain in the conservative near test so tiny
   floating-point camera-basis residue can only drop a boundary sprite, never
   leak a corner through the near plane. */


void SceneSpriteCentersPlane(const float* centers, const uint32_t* colors,
                             const float* half_sizes, const float* uv_rects,
                             const uint8_t* cells, int sprites,
                             int grid_log2, float inset,
                             float ux, float uy, float uz,
                             float vx, float vy, float vz) {
#if GLDC_SWAP_TELEMETRY
    const uint64_t spr_t0 = timer_us_gettime64();
#endif
    PolyList* out = _glActivePolyList();
    AlignedVector* sv = &out->sprites;

    pvr_sprite_hdr_t shdr;
    _glCompileCurrentSpriteHeader(out, &shdr);

    /* GL polygon-offset parity (AUD-001-OPA-09), hoisted once per call. */
    const float depth_mul = _glPolygonOffsetMul;

    float tu[3], tv[3], uw, vw;
    TransformVertex(ux, uy, uz, 0.0f, tu, &uw);
    TransformVertex(vx, vy, vz, 0.0f, tv, &vw);
    const float near_u_zw = tu[2] + uw;
    const float near_v_zw = tv[2] + vw;
    const float near_extent_scale =
        fabsf(near_u_zw) + fabsf(near_v_zw);

    /* Keep one compact bank per supported grid. HyperSolar alternates its 8x8
       glare atlas and 2x2 signal atlas every frame; a single last-used table
       regenerated both layouts forever. Heterogeneous banks cover all public
       grid sizes in 84 entries (only 20 more than the old 64-entry table). */
    static uint32_t cell_uv_2x2[4][3];
    static uint32_t cell_uv_4x4[16][3];
    static uint32_t cell_uv_8x8[64][3];
    static float cell_uv_inset[4];
    static uint8_t cell_uv_valid = 0;
    uint32_t (*cell_uv)[3] = NULL;
    int cell_mask = 0;
    if(cells) {
        const int grid = 1 << grid_log2;
        const float step = 1.0f / (float)grid;
        const uint8_t cache_bit = (uint8_t)(1u << grid_log2);
        switch(grid_log2) {
            case 1: cell_uv = cell_uv_2x2; break;
            case 2: cell_uv = cell_uv_4x4; break;
            case 3: cell_uv = cell_uv_8x8; break;
            default:
                /* The banks (and cell_uv_inset[4]) top out at 8x8: a larger
                   grid would overrun both (AUD-001-OPA-11). The public
                   wrapper guards 1..3; keep the internal contract loud. */
                gl_assert(0 && "grid_log2 out of range");
                cell_uv = cell_uv_8x8;
                break;
        }
        cell_mask = grid * grid - 1;
        if(!(cell_uv_valid & cache_bit) || inset != cell_uv_inset[grid_log2]) {
            for(int cell = 0; cell <= cell_mask; ++cell) {
                const int col = cell & (grid - 1);
                const int row = cell >> grid_log2;
                const float u0 = (float)col * step + inset;
                const float v0 = (float)row * step + inset;
                const float u1 = (float)(col + 1) * step - inset;
                const float v1 = (float)(row + 1) * step - inset;
                cell_uv[cell][0] = _glSpriteUV16(u0, v0);
                cell_uv[cell][1] = _glSpriteUV16(u1, v0);
                cell_uv[cell][2] = _glSpriteUV16(u1, v1);
            }
            cell_uv_inset[grid_log2] = inset;
            cell_uv_valid |= cache_bit;
        }
    }

    const uint32_t base_blocks = aligned_vector_size(sv);
#if GLDC_SWAP_TELEMETRY
    const uint32_t cap_before = aligned_vector_capacity(sv);
#endif
    uint32_t* const batch = (uint32_t*) aligned_vector_extend(
        sv, (uint32_t)sprites * 3u);  /* worst case: header + 64-byte sprite */
#if GLDC_SWAP_TELEMETRY
    if(aligned_vector_capacity(sv) != cap_before) _glSpriteGrowCount++;
#endif
    uint32_t used_blocks = 0;
    uint32_t last_argb = 0;
#if GLDC_SWAP_TELEMETRY
    uint32_t hdrs = 0;
#endif
    int have_hdr = 0;

    for(int q = 0; q < sprites; q += 2) {
        const int n = (q + 1 < sprites) ? 2 : 1;
        float tc[2][3], cw[2];
        const float* p = centers + q * 3;
        if(n == 2) {
            TransformVertex2(p[0], p[1], p[2], tc[0], &cw[0],
                             p[3], p[4], p[5], tc[1], &cw[1]);
        } else {
            TransformVertex(p[0], p[1], p[2], 1.0f, tc[0], &cw[0]);
        }

        for(int k = 0; k < n; ++k) {
            const int i = q + k;
            const float hs = half_sizes ? half_sizes[i] : 1.0f;
            const float near_extent = near_extent_scale * hs;
            if(tc[k][2] + cw[k] < near_extent)
                continue;

            const uint32_t argb = colors[i];
            if(!have_hdr || argb != last_argb) {
                uint32_t* h = batch + used_blocks * 8u;
                VERTEX_CACHE_ALLOC(h);
                _glWriteSpriteHeader(h, &shdr, argb);
                used_blocks++;
#if GLDC_SWAP_TELEMETRY
                hdrs++;
#endif
                last_argb = argb;
                have_hdr = 1;
            }

            pvr_sprite_txr_t* s =
                (pvr_sprite_txr_t*) (batch + used_blocks * 8u);
            used_blocks += 2;
            VERTEX_CACHE_ALLOC(s);
            VERTEX_CACHE_ALLOC((uint8_t*)s + 32);

            const float sux = tu[0] * hs;
            const float suy = tu[1] * hs;
            const float svx = tv[0] * hs;
            const float svy = tv[1] * hs;
            const float invw = _glFastInvert(cw[k]);
            /* GL polygon-offset parity with the Centers lane
               (AUD-001-OPA-09). depth_mul is hoisted: the float stores
               through the batch pointer would otherwise force GCC to
               re-load the global every sprite (may-alias). */
            const float z = ((cw[k] == 1.0f)
                ? _glFastInvert(1.0001f + tc[k][2]) : invw) * depth_mul;

            s->flags = GPU_CMD_VERTEX_EOL;
            s->ax = (tc[k][0] - sux - svx) * invw;
            s->ay = (tc[k][1] - suy - svy) * invw;
            s->az = z;
            s->bx = (tc[k][0] + sux - svx) * invw;
            s->by = (tc[k][1] + suy - svy) * invw;
            s->bz = z;
            s->cx = (tc[k][0] + sux + svx) * invw;
            s->cy = (tc[k][1] + suy + svy) * invw;
            s->cz = z;
            s->dx = (tc[k][0] - sux + svx) * invw;
            s->dy = (tc[k][1] - suy + svy) * invw;
            s->dummy = 0;
            if(cells) {
                const uint32_t* uv = cell_uv[cells[i] & cell_mask];
                s->auv = uv[0];
                s->buv = uv[1];
                s->cuv = uv[2];
            } else if(uv_rects) {
                const float* r = uv_rects + i * 4;
                s->auv = _glSpriteUV16(r[0], r[1]);
                s->buv = _glSpriteUV16(r[2], r[1]);
                s->cuv = _glSpriteUV16(r[2], r[3]);
            } else {
                s->auv = 0x00000000;
                s->buv = 0x3F800000;
                s->cuv = 0x3F803F80;
            }
        }
    }

    aligned_vector_resize(sv, base_blocks + used_blocks);
#if GLDC_SWAP_TELEMETRY
    _glSpriteHdrCount += hdrs;
    _glSpriteRecCount += (used_blocks - hdrs) >> 1;
    _glSpriteCallUs += (uint32_t)(timer_us_gettime64() - spr_t0);
#endif
}

/* SQ a finished sprite sidecar verbatim (records are pre-divided, pre-compiled).
   Self-contained register setup: the vertex submit may not have run this list. */
void SceneSpritesSubmit(void* blob, int blocks32) {
    if(blocks32 <= 0) return;

    GLDC_STAT_ADD(scene_sprite_records, (GLuint)blocks32);

    _glPrepareSubmissionRegisters();

    sq_dest_addr = (uintptr_t)SQ_MASK_DEST(PVR_TA_INPUT);
    const uintptr_t destination =
        _glOutputReserveRecords((size_t)blocks32);
    _glOutputCopyRecords(destination, blob, (size_t)blocks32);
    _glOutputWait();
}

/* Submit a validated list-major final command stream. Scene/list ownership
   stays with flush.c; this helper only performs the TA register setup and one
   contiguous store-queue copy. */
void SceneListSubmitFinal(const void* records, int record_count) {
    if(!records || record_count <= 0) return;
    _glPrepareSubmissionRegisters();
    sq_dest_addr = (uintptr_t)SQ_MASK_DEST(PVR_TA_INPUT);
    const uintptr_t destination =
        _glOutputReserveRecords((size_t)record_count);
    _glOutputCopyRecords(destination, records, (size_t)record_count);
    _glOutputWait();
}

static int _glScenePrepare(
        size_t op_records, size_t pt_records, size_t tr_records) {
#if GLDC_N4_VERTEX_DMA
    /* Stable-capacity N4 frames intentionally do NOT wait here. CPU final-
       record construction proceeds in KOS's inactive RAM half while the TA
       consumes the previous one; pvr_scene_finish() performs the readiness
       wait immediately before launching this scene's DMA chain. Buffer growth
       and global fog-register updates are the two operations that still need
       an explicit safe TA boundary. */
    const bool growth = _glN4BuffersNeedGrowth(
        op_records, pt_records, tr_records);
    if((growth || _glN4FogPending()) && pvr_wait_ready() < 0) return -1;
    if(growth) _glN4EnsureBuffers(op_records, pt_records, tr_records);
    /* The first N4 scene necessarily grows from zero and therefore just
       crossed a safe TA boundary. Program GLdc's stable TA input mode once;
       later list writers only build RAM and never touch live PVR registers. */
    _glPrepareSubmissionRegisters();
#else
    (void)op_records;
    (void)pt_records;
    (void)tr_records;
    if(pvr_wait_ready() < 0) return -1;
#endif
    ApplyDeferredFogTable();
    return 0;
}

int SceneBeginSizedChecked(
        size_t op_records, size_t pt_records, size_t tr_records) {
    if(_glScenePrepare(op_records, pt_records, tr_records) < 0) return -1;
    pvr_scene_begin();
    return 0;
}

int SceneBeginChecked(void) {
#if GLDC_N4_VERTEX_DMA
    fprintf(stderr,
            "GLdc N4: unsized SceneBeginChecked is not a valid DMA scene"
            " boundary\n");
    _glSubmissionFatal();
#else
    return SceneBeginSizedChecked(0u, 0u, 0u);
#endif
}

void SceneBeginSized(
        size_t op_records, size_t pt_records, size_t tr_records) {
    if(SceneBeginSizedChecked(op_records, pt_records, tr_records) < 0) {
        fprintf(stderr, "GLdc: PVR scene preparation failed\n");
        _glSubmissionFatal();
    }
}

void SceneBegin() {
#if GLDC_N4_VERTEX_DMA
    fprintf(stderr,
            "GLdc N4: unsized SceneBegin is not a valid DMA scene boundary\n");
    _glSubmissionFatal();
#else
    SceneBeginSized(0u, 0u, 0u);
#endif
}

/* Like SceneBegin, but renders this scene into a texture in VRAM instead of the
   framebuffer (KOS render-to-texture). Used by glKosFlushToTexture for the
   two-pass HUD overlay: pass 1 renders the world into a texture, pass 2 draws
   that texture + the HUD to the screen so the HUD composites on top of
   everything. w/h are the (power-of-two) target dimensions. */
void SceneBeginToTextureSized(
        void* tex, unsigned int w, unsigned int h,
        size_t op_records, size_t pt_records, size_t tr_records) {
    if(_glScenePrepare(op_records, pt_records, tr_records) < 0) {
        fprintf(stderr, "GLdc: PVR render-to-texture preparation failed\n");
        _glSubmissionFatal();
    }
    /* stride == w: the target is a tightly-packed power-of-two texture */
    if(pvr_scene_begin_rtt((pvr_ptr_t)tex, w, h, w) < 0) {
        fprintf(stderr, "GLdc: invalid render-to-texture scene target\n");
        _glSubmissionFatal();
    }
}

void SceneBeginToTexture(void* tex, unsigned int w, unsigned int h) {
#if GLDC_N4_VERTEX_DMA
    (void)tex;
    (void)w;
    (void)h;
    fprintf(stderr,
            "GLdc N4: unsized SceneBeginToTexture is not a valid DMA scene"
            " boundary\n");
    _glSubmissionFatal();
#else
    SceneBeginToTextureSized(tex, w, h, 0u, 0u, 0u);
#endif
}

int SceneListBeginChecked(GPUList list) {
    const int result = pvr_list_begin(list);
#if GLDC_N4_VERTEX_DMA
    if(result == 0) {
        gl_assert(!n4_output_open);
        gl_assert(list == GPU_LIST_OP_POLY ||
                  list == GPU_LIST_PT_POLY ||
                  list == GPU_LIST_TR_POLY);
        const int slot = _glN4ListSlot(list);
        gl_assert(slot >= 0);
        if(unlikely(slot < 0)) {
            fprintf(stderr, "GLdc N4: unsupported DMA list %d\n", (int)list);
            _glSubmissionFatal();
        }
        n4_output_list = list;
        n4_output_start = (uintptr_t)pvr_vertbuf_tail((pvr_list_t)list);
        n4_output_cursor = n4_output_start;
        n4_output_limit = n4_output_start +
            n4_dma_buffers[slot].frame_bytes -
            GLDC_N4_KOS_TAIL_RECORDS * sizeof(Vertex);
        n4_output_open = true;
    }
#endif
    /* pvr_list_begin acquires the store queues and programs QACR for TA input.
       pvr_dr_init is a deprecated no-op in current KOS. In N4 the configured
       list uses cached RAM instead, so no SQ lock/QACR ownership is expected. */
    return result;
}

void SceneListBegin(GPUList list) {
    (void)SceneListBeginChecked(list);
}

int SceneListFinishChecked(void) {
#if GLDC_N4_VERTEX_DMA
    gl_assert(n4_output_open);
    if(!n4_output_open) return -1;
    const size_t bytes = (size_t)(n4_output_cursor - n4_output_start);
    pvr_vertbuf_written((pvr_list_t)n4_output_list, bytes);
    n4_output_open = false;
    n4_output_cursor = n4_output_limit = n4_output_start = 0u;
#endif
    return pvr_list_finish();
}

void SceneListFinish() {
    (void)SceneListFinishChecked();
}

int SceneFinishChecked(void) {
#if GLDC_N4_VERTEX_DMA
    gl_assert(!n4_output_open);
#endif
    return pvr_scene_finish();
}

void SceneFinish() {
    (void)SceneFinishChecked();
}

int SceneTextureFence(void) {
    /* Caller has excluded an open/unfinished scene. Waiting only for render
       done misses a closed TA scene that has not started rendering yet.
       KOS clears ta_busy AFTER starting that render, then clears render_busy
       at render completion. Both waits propagate their timeout failures. */
    if(pvr_wait_ready() < 0) return -1;
    return pvr_wait_render_done();
}

const VideoMode* GetVideoMode() {
    static VideoMode mode;
    mode.width = vid_mode->width;
    mode.height = vid_mode->height;
    return &mode;
}

#if GLDC_S3_SEGMENTED_OP
/* ---- S3 segmented hot drain (2026-07-23, HyperSolar ledger A3) -------------
   The OP vector is READ-ONLY here: drains submit [cursor..size) through the
   normal SceneListSubmit (run finalizer + the exact clip fallback), so capture/
   replay spans, record layout and clipping behavior stay untouched. The scene
   and the OP list open lazily at the FIRST drain — the pvr_wait_ready that used
   to sit in swap happens there instead — and OP stays open until swap (a PVR
   list can only begin once per scene). Drain boundaries are draw boundaries, so
   strips never split; the TA keeps header state within the open list, so
   headerless follow-up segments are fine. */
static int      s3_scene_open = 0;
static int      s3_op_open = 0;
static uint32_t s3_op_drained = 0;
uint64_t        _glS3DrainUs = 0;    /* summed drain time; snapshot at HT1 window */

int _glS3SceneOpen(void) { return s3_scene_open; }

void _glS3SwapReset(void) {
    s3_scene_open = 0;
    s3_op_open = 0;
    s3_op_drained = 0;
}

/* Re-arm what sq_lock programs for the TA: another store-queue user between
   segments (texture upload etc.) would have retargeted QACR. Two stores, free. */
static inline void s3_arm_qacr(void) {
    *((volatile uint32_t*)0xFF000038) = ((uintptr_t)PVR_TA_INPUT >> 24) & 0x1C;  /* QACR0 */
    *((volatile uint32_t*)0xFF00003C) = ((uintptr_t)PVR_TA_INPUT >> 24) & 0x1C;  /* QACR1 */
}

static void s3_ensure_op_open(void) {
    if(!s3_scene_open) {
        SceneBegin();
        s3_scene_open = 1;
    }
    if(!s3_op_open) {
        SceneListBegin(GPU_LIST_OP_POLY);
        s3_op_open = 1;
    }
    s3_arm_qacr();
}

void _glS3DrainOP(void) {
    PolyList* l = _glOpaquePolyList();
    const uint32_t size = aligned_vector_size(&l->vector);
    /* < 4 records can't render (header + a 3-vert strip); leave the fragment
       for a later drain or the swap tail — the cursor holds, nothing is lost. */
    if(size - s3_op_drained < 4) return;
    /* OPPORTUNISTIC scene open: NEVER block mid-frame on the previous render.
       On light scenes the PVR isn't ready until near the flip, so everything
       defers to the swap — byte-identical legacy pacing and clean draw/swap
       counters. On heavy scenes the previous render finishes mid-frame and the
       drains kick in exactly when they're free. (First-boot S3 opened the scene
       unconditionally and moved stage 1's ~14ms of vsync idle into the DRAW
       timer — the fence belongs at swap unless draining is actually free.) */
    if(!s3_scene_open && pvr_check_ready() < 0) return;
    const uint64_t t0 = timer_us_gettime64();
    s3_ensure_op_open();
    SceneListSubmit((Vertex*)aligned_vector_at(&l->vector, s3_op_drained),
                    (int)(size - s3_op_drained));
    s3_op_drained = size;
    _glS3DrainUs += timer_us_gettime64() - t0;
}

/* Swap-side: submit whatever OP content the frame left undrained, with the
   relaxed guard a tiny headerless tail needs (the TA already holds the header
   state from the previous segment; the generic path EOL-terminates exactly). */
void _glS3SubmitOpTail(void) {
    PolyList* l = _glOpaquePolyList();
    const uint32_t size = aligned_vector_size(&l->vector);
    const uint32_t n = size - s3_op_drained;
    if(n == 0) return;
    s3_ensure_op_open();
    Vertex* start = (Vertex*)aligned_vector_at(&l->vector, s3_op_drained);
    if(n >= 4) {
        SceneListSubmit(start, (int)n);
    } else {
        _glPrepareSubmissionRegisters();
        sq_dest_addr = (uintptr_t)SQ_MASK_DEST(PVR_TA_INPUT);
        SceneListSubmitGeneric(start, (int)n, false);
        sq_wait();
    }
    s3_op_drained = size;
}

/* Swap needs to know whether OP must still be opened for its sprite sidecar
   when the frame never drained (no scene open -> legacy path handles it). */
int _glS3OpOpen(void) { return s3_op_open; }
#endif /* GLDC_S3_SEGMENTED_OP */
