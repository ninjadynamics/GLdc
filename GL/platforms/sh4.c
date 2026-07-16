#include <float.h>

#include <dc/sq.h>

#include "../platform.h"
#include "sh4.h"
#include "../gldc_stats.h"


#define CLIP_DEBUG 0

/* Shared PVR vertex buffer (all lists, one frame). Enlarged from the stock 2560*256
   (655,360 B = 20,480 verts): the HyperSolar city stage submits its whole geometry TWICE
   under CITY_DOUBLE_PASS (opaque base + punch-through windows), which overran the old buffer
   and stalled the TA -> hard hang / dcload reset. 6144*256 = 1,572,864 B = 49,152 verts holds
   2x the capped city (CITY_SUBMIT_VCAP) plus the rest of the scene. Costs ~917 KB extra VRAM
   over stock (of 8 MB; framebuffers + VQ textures leave room). The game also caps the city
   submission as a hard backstop, so this size is headroom, not a hard dependency. */
#define PVR_VERTEX_BUF_SIZE 6144 * 256
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
    float amount;
    float a;
    float r;
    float g;
    float b;
    float start;
    float end;
    float far_depth;
} deferredFog;

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

GL_FORCE_INLINE bool glIsVertex(const float flags) {
    return flags == GPU_CMD_VERTEX_EOL || flags == GPU_CMD_VERTEX;
}

GL_FORCE_INLINE bool glIsLastVertex(const float flags) {
    return flags == GPU_CMD_VERTEX_EOL;
}

void InitGPU(_Bool autosort, _Bool fsaa) {
    pvr_init_params_t params = {
        /* Bin sizes: opaque, op_modifier, translucent, tr_modifier, punch-through.
           KOS caps bin size at _32. */
        {PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32},
        PVR_VERTEX_BUF_SIZE, /* Vertex buffer size */
        0, /* No DMA */
        fsaa, /* No FSAA */
        (autosort) ? 0 : 1, /* Disable translucent auto-sorting to match traditional GL */
        PVR_OPB_COUNT /* Number of tile object pointer overflow bins. */
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

static uintptr_t sq_dest_addr = 0;

static inline void _glPushHeaderOrVertex(Vertex* v, size_t count)  {
    TRACE();

#if CLIP_DEBUG
    fprintf(stderr, "{%f, %f, %f, %f}, // %x (%x)\n", v->xyz[0], v->xyz[1], v->xyz[2], v->w, v->flags, v);
#endif

    sq_fast_cpy((void *)sq_dest_addr, v, count);
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
    vout->xyz[2] = (vout->xyz[2] < FLT_EPSILON) ? FLT_EPSILON : vout->xyz[2];

    vout->uv[0] = invt * v1->uv[0] + t * v2->uv[0];
    vout->uv[1] = invt * v1->uv[1] + t * v2->uv[1];

    vout->w = invt * v1->w + t * v2->w;

    vout->bgra[0] = invt * v1->bgra[0] + t * v2->bgra[0];
    vout->bgra[1] = invt * v1->bgra[1] + t * v2->bgra[1];
    vout->bgra[2] = invt * v1->bgra[2] + t * v2->bgra[2];
    vout->bgra[3] = invt * v1->bgra[3] + t * v2->bgra[3];
}

#define SPAN_SORT_CFG 0x005F8030
static volatile uint32_t* PVR_LMMODE0 = (uint32_t*) 0xA05F6884;
static volatile uint32_t *PVR_LMMODE1 = (uint32_t*) 0xA05F6888;

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

/* The ORIGINAL per-triangle submission loop, kept bit-for-bit as the exact
   fallback for strips that cross the near plane (or are malformed). The fast
   wrapper below feeds it single header-less strip spans, so the minimum
   renderable span here is 3 vertices, not header+3. */
static void SceneListSubmitGeneric(Vertex* vertices, int n) {
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
    do { if(queued_vertex) { queued_vertex->flags = (sflags); _glPushHeaderOrVertex(queued_vertex, 1); queued_vertex = NULL; } } while(0)

    int visible_mask = 0;

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
            _glPushHeaderOrVertex(v0, 1);
            visible_mask = 0;
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
                _glPushHeaderOrVertex(v0, 2);
            } else {
                // If the previous triangle wasn't all visible, and we
                // queued a vertex - we force it to be EOL and submit
                SUBMIT_QUEUED_VERTEX(GPU_CMD_VERTEX_EOL);
            }

            i++;
            v0++;
            visible_mask = 0;
            continue;
        }

        visible_mask = (
            (v0->xyz[2] >= -v0->w) << 0 |
            (v1->xyz[2] >= -v1->w) << 1 |
            (v2->xyz[2] >= -v2->w) << 2
        );

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
            break;
            case FIRST_VISIBLE:
                _glClipEdge(v0, v1, a);
                a->flags = GPU_CMD_VERTEX;

                _glClipEdge(v2, v0, b);
                b->flags = GPU_CMD_VERTEX;

                _glPerspectiveDivideVertex(v0, 1);
                _glPushHeaderOrVertex(v0, 1);

                _glPerspectiveDivideVertex(a, 2);
                _glPushHeaderOrVertex(a, 2);

                QUEUE_VERTEX(b);
            break;
            case SECOND_VISIBLE:
                memcpy_vertex(c, v1);

                _glClipEdge(v0, v1, a);
                a->flags = GPU_CMD_VERTEX;

                _glClipEdge(v1, v2, b);
                b->flags = v2->flags;

                _glPerspectiveDivideVertex(a, 3);
                _glPushHeaderOrVertex(a, 1);

                _glPushHeaderOrVertex(c, 1);

                QUEUE_VERTEX(b);
            break;
            case THIRD_VISIBLE:
                memcpy_vertex(c, v2);

                _glClipEdge(v1, v2, a);
                a->flags = GPU_CMD_VERTEX;

                _glClipEdge(v2, v0, b);
                b->flags = GPU_CMD_VERTEX;

                _glPerspectiveDivideVertex(a, 3);
                _glPushHeaderOrVertex(a, 2);

                QUEUE_VERTEX(c);
            break;
            case FIRST_AND_SECOND_VISIBLE:
                memcpy_vertex(c, v1);

                _glClipEdge(v2, v0, b);
                b->flags = GPU_CMD_VERTEX;

                _glPerspectiveDivideVertex(v0, 1);
                _glPushHeaderOrVertex(v0, 1);

                _glClipEdge(v1, v2, a);
                a->flags = v2->flags;

                _glPerspectiveDivideVertex(a, 3);

                _glPushHeaderOrVertex(c, 1);

                _glPushHeaderOrVertex(b, 2);

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
                _glPushHeaderOrVertex(a, 1);

                _glPushHeaderOrVertex(c, 1);

                _glPushHeaderOrVertex(b, 2);

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
                _glPushHeaderOrVertex(v0, 1);

                _glPerspectiveDivideVertex(a, 3);
                _glPushHeaderOrVertex(a, 1);

                _glPushHeaderOrVertex(c, 1);

                _glPushHeaderOrVertex(b, 1);

                QUEUE_VERTEX(c);
            break;
            default:
                fprintf(stderr, "ERROR\n");
        }
    }

    SUBMIT_QUEUED_VERTEX(GPU_CMD_VERTEX_EOL);

    sq_wait();
}

/* ---- All-visible run finalizer (2026-07-15, HyperSolar investigation) ----
   [GLDC-T] proved the old submission loop IS the swap cost (wait=0.01ms, op+tr
   ~6.5ms at heavy city load): every vertex paid a 32-byte qv staging copy plus
   a count=1 sq_fast_cpy (FSCHG entry/exit per record, always queue SQ0). But
   near-plane clipping is RARE — almost every strip in a frame is fully
   visible. So: scan the list at STRIP granularity; divide all-visible strips
   in place; accumulate maximal runs (headers ride along untouched — they are
   plain 32-byte records); submit each run with ONE multi-record sq_fast_cpy
   (KOS's asm alternates SQ0/SQ1 internally and pays FSCHG once per call).
   Any strip that crosses the near plane (or lacks an EOL terminator) flushes
   the run and takes SceneListSubmitGeneric above — the byte-exact old path. */
void SceneListSubmit(Vertex* vertices, int n) {
    TRACE();

    /* You need at least a header, and 3 vertices to render anything */
    if(n < 4) {
        return;
    }

    GLDC_STAT_INC(scene_list_submits);
    GLDC_STAT_ADD(scene_vertices_in, n);

    PVR_SET(SPAN_SORT_CFG, 0x0);
    *PVR_LMMODE0 = 0;
    *PVR_LMMODE1 = 0;

    sq_dest_addr = (uintptr_t)SQ_MASK_DEST(PVR_TA_INPUT);

    Vertex* v = vertices;
    Vertex* const vend = vertices + n;
    Vertex* run_start = v;

    while(v < vend) {
        if(is_header(v)) {
            GLDC_STAT_INC(scene_headers_seen);
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

        if(all_visible) {
            _glPerspectiveDivideVertex(v, (int)(strip_end - v) + 1);
            v = strip_end + 1;        /* strip stays in the run */
        } else {
            /* Flush everything accumulated before this strip, then let the
               exact old path do the clip work on the strip alone. */
            if(v > run_start) {
                sq_fast_cpy((void*) sq_dest_addr, run_start, (size_t)(v - run_start));
            }
            SceneListSubmitGeneric(v, (int)(strip_end - v) + 1);
            v = strip_end + 1;
            run_start = v;
        }
    }

    if(v > run_start) {
        sq_fast_cpy((void*) sq_dest_addr, run_start, (size_t)(v - run_start));
    }

    sq_wait();
}

void SceneBegin() {
    pvr_wait_ready();
    ApplyDeferredFogTable();
    pvr_scene_begin();
}

/* Like SceneBegin, but renders this scene into a texture in VRAM instead of the
   framebuffer (KOS render-to-texture). Used by glKosFlushToTexture for the
   two-pass HUD overlay: pass 1 renders the world into a texture, pass 2 draws
   that texture + the HUD to the screen so the HUD composites on top of
   everything. w/h are the (power-of-two) target dimensions. */
void SceneBeginToTexture(void* tex, unsigned int w, unsigned int h) {
    uint32_t rx = w, ry = h;
    pvr_wait_ready();
    ApplyDeferredFogTable();
    pvr_scene_begin_txr((pvr_ptr_t) tex, &rx, &ry);
}

static pvr_dr_state_t dr_state;
void SceneListBegin(GPUList list) {
    pvr_list_begin(list);
    /* Direct rendering auto acquires/releases store queue */
    pvr_dr_init(&dr_state);
}

void SceneListFinish() {
    pvr_list_finish();
}

void SceneFinish() {
    pvr_scene_finish();
}

const VideoMode* GetVideoMode() {
    static VideoMode mode;
    mode.width = vid_mode->width;
    mode.height = vid_mode->height;
    return &mode;
}
