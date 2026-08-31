
#include "../containers/aligned_vector.h"
#include "private.h"
#include "config.h"
#include "gldc_stats.h"

#if defined(GLDC_NATIVE_BENCH) && GLDC_NATIVE_BENCH
#include <dc/pvr.h>
#endif

#if GLDC_S3_SEGMENTED_OP
/* S3 segmented hot drain — platforms/sh4.c */
int  _glS3SceneOpen(void);
int  _glS3OpOpen(void);
void _glS3SwapReset(void);
void _glS3SubmitOpTail(void);
extern uint64_t _glS3DrainUs;
#endif

PolyList OP_LIST;
PolyList PT_LIST;
PolyList TR_LIST;

/**
 *  FAST_MODE will use invW for all Z coordinates sent to the
 *  GPU.
 *
 *  This will break orthographic mode so default is FALSE
 **/

#define FAST_MODE GL_FALSE

GLboolean AUTOSORT_ENABLED = GL_FALSE;

PolyList* _glOpaquePolyList() {
    return &OP_LIST;
}

PolyList* _glPunchThruPolyList() {
    return &PT_LIST;
}

PolyList *_glTransparentPolyList() {
    return &TR_LIST;
}

void APIENTRY glFlush() {

}

void APIENTRY glFinish() {

}


void APIENTRY glKosInitConfig(GLdcConfig* config) {
    config->autosort_enabled = GL_FALSE;
    config->fsaa_enabled = GL_FALSE;

    config->initial_op_capacity = 1024 * 4;
    config->initial_pt_capacity = 512 * 4;
    config->initial_tr_capacity = 1024 * 4;
    config->initial_immediate_capacity = 1024 * 4;

    // RGBA4444 is the fastest general format - 8888 will cause a perf issue
    config->internal_palette_format = GL_RGBA4;

    config->texture_twiddle = GL_TRUE;
}

static bool _initialized = false;

void APIENTRY glKosInitEx(GLdcConfig* config) {
    if(_initialized) {
        return;
    }

    _initialized = true;

    TRACE();

    printf("\nGLdc: [ CANARY ] Welcome to MODIFIED LOCAL GLdc! Git revision: %s [2026.09.01-0023-stats0-glt1-nbench0-n30-n4dma0-zamv070]\n", GLDC_VERSION);

#ifdef USE_SH4ZAM
    printf("GLdc: Hello SH4ZAM!\n\n");
#else
    printf("GLdc: SH4ZAM is NOT enabled.\n\n");
#endif
#if GLDC_GOLD_BLOCK
    printf("GLdc: [ CANARY ] GOLD-BLOCK quad writer ENABLED (B2)\n");
#endif
#if GLDC_S3_SEGMENTED_OP
    printf("GLdc: [ CANARY ] S3 segmented OP drain ENABLED\n");
#endif
#if GLDC_N4_VERTEX_DMA
    printf("GLdc: [ CANARY ] N4 direct KOS vertex-DMA sink ENABLED\n");
#endif

    InitGPU(config->autosort_enabled, config->fsaa_enabled);

    AUTOSORT_ENABLED = config->autosort_enabled;

    _glInitSubmissionTarget();
    _glInitMatrices();
    _glInitAttributePointers();
    _glInitContext();
    _glInitLights();
    _glInitImmediateMode(config->initial_immediate_capacity);
    _glInitFramebuffers();

    _glSetInternalPaletteFormat(config->internal_palette_format);

    _glInitTextures();

    if(config->texture_twiddle) {
        glEnable(GL_TEXTURE_TWIDDLE_KOS);
    }

    OP_LIST.list_type = GPU_LIST_OP_POLY;
    PT_LIST.list_type = GPU_LIST_PT_POLY;
    TR_LIST.list_type = GPU_LIST_TR_POLY;

    aligned_vector_init(&OP_LIST.vector, sizeof(Vertex));
    aligned_vector_init(&PT_LIST.vector, sizeof(Vertex));
    aligned_vector_init(&TR_LIST.vector, sizeof(Vertex));

    aligned_vector_reserve(&OP_LIST.vector, config->initial_op_capacity);
    aligned_vector_reserve(&PT_LIST.vector, config->initial_pt_capacity);
    aligned_vector_reserve(&TR_LIST.vector, config->initial_tr_capacity);

    /* Sprite sidecars (32B units: compiled headers + 64B sprite records) */
    aligned_vector_init(&OP_LIST.sprites, 32);
    aligned_vector_init(&PT_LIST.sprites, 32);
    aligned_vector_init(&TR_LIST.sprites, 32);
    /* 3072, not 512: HyperSolar's busy city frames legitimately reach ~3k
       blocks (pools + glare orbs + traffic + signals, GLOW_FACE_CAP 1024 per
       batch). A mid-frame aligned_vector growth memcpys the whole accumulated
       lane — measured as multi-ms submit spikes. The vector keeps its peak
       capacity anyway, so reserving it up front costs the same RAM and never
       copies. (Bruno 2026-08-04 lamp-budget dissection.) */
    aligned_vector_reserve(&TR_LIST.sprites, 3072);  /* the glow lane lives on TR */
#ifdef _arch_dreamcast
    _glResetDeferredP3T2BGRA();
    _glInitPvrPackets();
#endif
}

extern void _glInvalidateCapturedArrays(void);  /* draw.c: captures die with the cleared lists */
extern void _glResetDeferredFrees(void);        /* texture.c: drop queued records, no frees */
extern void _glShutdownTextures(void);          /* texture.c: palettes + object array + pool bookkeeping */
extern void _glShutdownFramebuffers(void);      /* framebuffer.c: object array */
extern void _glShutdownImmediateMode(void);     /* immediate.c: vertex staging vector */

void APIENTRY glKosShutdown() {
    /* cleanup, not clear: glKosInitEx re-runs aligned_vector_init, which
       would orphan the old buffers (~420 KB per shutdown/init cycle). The
       header flags must drop with them or the first draw after re-init
       would skip its poly header (review F2/F3). */
    aligned_vector_cleanup(&OP_LIST.vector);
    aligned_vector_cleanup(&PT_LIST.vector);
    aligned_vector_cleanup(&TR_LIST.vector);
    aligned_vector_cleanup(&OP_LIST.sprites);
    aligned_vector_cleanup(&PT_LIST.sprites);
    aligned_vector_cleanup(&TR_LIST.sprites);
    OP_LIST.header_emitted = GL_FALSE;
    PT_LIST.header_emitted = GL_FALSE;
    TR_LIST.header_emitted = GL_FALSE;
#ifdef _arch_dreamcast
    _glResetDeferredP3T2BGRA();
    _glShutdownPvrPackets();
#endif

    _glShutdownImmediateMode();

    _glInvalidateCapturedArrays();
    _glResetDeferredFrees();   /* ShutdownGPU tears the whole VRAM heap down anyway */

    /* A shutdown/init cycle used to leak the texture/framebuffer object
       arrays, every palette, and the allocator bookkeeping (AUD-001-OPB-27). */
    _glShutdownTextures();
    _glShutdownFramebuffers();

    ShutdownGPU();
    _initialized = false;
}

void APIENTRY glKosInit() {
    GLdcConfig config;
    glKosInitConfig(&config);
    glKosInitEx(&config);
}

extern void _glProcessDeferredFrees(void);   /* texture.c: aged texture-VRAM release */

/* Swap-time decomposition (2026-07-15 investigation): the game's `swap=` telemetry lumps
   pvr_wait_ready (previous-frame PVR wait) together with the three SceneListSubmit walks
   and scene finish — a submission win is invisible until these are split. Rate-limited
   aggregate snapshot is taken and reset by the game's compact telemetry
   window; near-zero cost otherwise.
   GLDC_SWAP_TELEMETRY lives in config.h (sh4.c's sprite-lane timers share it —
   Audit #001 OPA-12); 0 compiles the sampling out entirely. */
#if GLDC_SWAP_TELEMETRY
#include <arch/timer.h>
#include <stdio.h>
static uint64_t _gt_wait_us, _gt_op_us, _gt_pt_us, _gt_tr_us, _gt_fin_us;
static uint64_t _gt_op_verts, _gt_tr_verts;   /* walked records: op= scales with these alone */
extern uint32_t _glSpriteHdrCount, _glSpriteRecCount;   /* sprite-lane split (sh4.c) */
extern uint32_t _glSpriteCallUs, _glSpriteGrowCount;    /* lamp-budget dissection (sh4.c) */
static int _gt_frames;
#define GT_MARK(var, expr) do { \
        uint64_t _t0 = timer_us_gettime64(); \
        expr; \
        var += timer_us_gettime64() - _t0; \
    } while(0)
#else
#define GT_MARK(var, expr) expr
#endif

GLboolean APIENTRY glKosTakeSwapTelemetry(
        GLKosSwapTelemetry *out, GLuint out_size) {
    if(!out || out_size < sizeof(*out)) return GL_FALSE;
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->abi_version = GL_KOS_SWAP_TELEMETRY_ABI_VERSION;
#if GLDC_SWAP_TELEMETRY
    out->frames = (GLuint)_gt_frames;
    out->wait_us = _gt_wait_us;
#if GLDC_S3_SEGMENTED_OP
    out->s3_us = _glS3DrainUs;
#endif
    out->op_us = _gt_op_us;
    out->pt_us = _gt_pt_us;
    out->tr_us = _gt_tr_us;
    out->finish_us = _gt_fin_us;
    out->op_vertices = _gt_op_verts;
    out->tr_vertices = _gt_tr_verts;
    out->sprite_headers = _glSpriteHdrCount;
    out->sprite_records = _glSpriteRecCount;
    out->sprite_call_us = _glSpriteCallUs;
    out->sprite_grows = _glSpriteGrowCount;

    _gt_wait_us = _gt_op_us = _gt_pt_us = _gt_tr_us = _gt_fin_us = 0;
    _gt_op_verts = _gt_tr_verts = 0;
#if GLDC_S3_SEGMENTED_OP
    _glS3DrainUs = 0;
#endif
    _glSpriteHdrCount = _glSpriteRecCount = 0;
    _glSpriteCallUs = _glSpriteGrowCount = 0;
    _gt_frames = 0;
    return GL_TRUE;
#else
    return GL_FALSE;
#endif
}

/* One list's full submission: vertex stream then the sprite sidecar. Tail
   placement is order-independent for additive sprites. Alpha-blended sprites
   are permitted only as an explicit tail layer and must be appended after any
   earlier sidecar family they are meant to cover. Begin/finish are the
   caller's — it decides whether the list opens at all. */
static void submit_list(PolyList* l) {
    const GLboolean has_deferred =
#ifdef _arch_dreamcast
        _glDeferredP3T2BGRAListCount(l) > 0 ||
        _glPvrPacketListCount(l) > 0;
#else
        GL_FALSE;
#endif
    if(aligned_vector_header(&l->vector)->size > 2 || has_deferred) {
        SceneListSubmit((Vertex*) aligned_vector_front(&l->vector), aligned_vector_size(&l->vector));
    }
    const uint32_t sn = aligned_vector_size(&l->sprites);
    if(sn) {
        SceneSpritesSubmit(aligned_vector_front(&l->sprites), (int) sn);
    }
}

static GLboolean list_has_content(PolyList* l) {
    if(aligned_vector_header(&l->vector)->size > 2 ||
       aligned_vector_size(&l->sprites) > 0) return GL_TRUE;
#ifdef _arch_dreamcast
    if(_glDeferredP3T2BGRAListCount(l) > 0) return GL_TRUE;
    if(_glPvrPacketListCount(l) > 0) return GL_TRUE;
#endif
    return GL_FALSE;
}

#if GLDC_N4_VERTEX_DMA
static size_t n4_list_record_budget(PolyList* l) {
    const int records = (int)aligned_vector_size(&l->vector);
    const int sprites = (int)aligned_vector_size(&l->sprites);
    const Vertex* const vertices = records > 0
        ? (const Vertex*)aligned_vector_front(&l->vector) : NULL;
    return SceneListRecordBudget(vertices, records, sprites);
}

static void n4_begin_queued_scene(void) {
    SceneBeginSized(
        n4_list_record_budget(&OP_LIST),
        n4_list_record_budget(&PT_LIST),
        n4_list_record_budget(&TR_LIST));
}

static void n4_begin_queued_scene_to_texture(
        void* tex, unsigned int w, unsigned int h) {
    SceneBeginToTextureSized(
        tex, w, h,
        n4_list_record_budget(&OP_LIST),
        n4_list_record_budget(&PT_LIST),
        n4_list_record_budget(&TR_LIST));
}
#endif

static void clear_lists(void) {
    aligned_vector_clear(&OP_LIST.vector);
    aligned_vector_clear(&PT_LIST.vector);
    aligned_vector_clear(&TR_LIST.vector);
    aligned_vector_clear(&OP_LIST.sprites);
    aligned_vector_clear(&PT_LIST.sprites);
    aligned_vector_clear(&TR_LIST.sprites);
    OP_LIST.header_emitted = GL_FALSE;
    PT_LIST.header_emitted = GL_FALSE;
    TR_LIST.header_emitted = GL_FALSE;
#ifdef _arch_dreamcast
    _glResetDeferredP3T2BGRA();
    _glResetPvrPackets();
#endif
}

static void finish_exclusive_state(GLboolean successful_scene) {
    clear_lists();
#if GLDC_S3_SEGMENTED_OP
    _glS3SwapReset();
#endif
    _glApplyScissor(true);
    if(successful_scene) {
        _glProcessDeferredFrees();
    }
    _glInvalidateCapturedArrays();
    _glGPUStateMarkDirty();
}

GLint APIENTRY glKosPvrSubmitExclusiveScene(
        const GLKosPvrExclusiveScene* scene) {
#ifndef _arch_dreamcast
    (void)scene;
    return GL_KOS_PVR_UNSUPPORTED_STATE;
#else
    if(!scene) return GL_KOS_PVR_BAD_ARGUMENT;
    if(!_initialized) return GL_KOS_PVR_UNSUPPORTED_STATE;

    const GLint op = _glPvrValidateExclusiveList(
        &scene->opaque, GPU_LIST_OP_POLY);
    const GLint pt = _glPvrValidateExclusiveList(
        &scene->punch_through, GPU_LIST_PT_POLY);
    const GLint tr = _glPvrValidateExclusiveList(
        &scene->translucent, GPU_LIST_TR_POLY);
    if(op != GL_KOS_PVR_OK || pt != GL_KOS_PVR_OK ||
       tr != GL_KOS_PVR_OK) {
        GLDC_STAT_INC(pvr_exclusive_rejects);
        if(op != GL_KOS_PVR_OK) return op;
        if(pt != GL_KOS_PVR_OK) return pt;
        return tr;
    }
    if(scene->opaque.record_count == 0 &&
       scene->punch_through.record_count == 0 &&
       scene->translucent.record_count == 0) {
        GLDC_STAT_INC(pvr_exclusive_rejects);
        return GL_KOS_PVR_BAD_ARGUMENT;
    }

#if GLDC_S3_SEGMENTED_OP
    if(_glS3SceneOpen()) {
        GLDC_STAT_INC(pvr_exclusive_rejects);
        return GL_KOS_PVR_QUEUE_NOT_EMPTY;
    }
#endif
    if(!_glPvrPacketExclusiveReady() ||
       aligned_vector_size(&OP_LIST.vector) != 0 ||
       aligned_vector_size(&PT_LIST.vector) != 0 ||
       aligned_vector_size(&TR_LIST.vector) != 0 ||
       aligned_vector_size(&OP_LIST.sprites) != 0 ||
       aligned_vector_size(&PT_LIST.sprites) != 0 ||
       aligned_vector_size(&TR_LIST.sprites) != 0 ||
       _glDeferredP3T2BGRACount() != 0 || _glPvrPacketCount() != 0) {
        GLDC_STAT_INC(pvr_exclusive_rejects);
        return GL_KOS_PVR_QUEUE_NOT_EMPTY;
    }

#if GLDC_N4_VERTEX_DMA
    if(SceneBeginSizedChecked(
           (size_t)scene->opaque.record_count,
           (size_t)scene->punch_through.record_count,
           (size_t)scene->translucent.record_count) < 0) {
#else
    if(SceneBeginChecked() < 0) {
#endif
        GLDC_STAT_INC(pvr_exclusive_rejects);
        return GL_KOS_PVR_PVR_ERROR;
    }
    GLboolean scene_started = GL_TRUE;
    GLuint lists = 0;
    GLuint records = 0;
    if(scene->opaque.record_count > 0) {
        if(SceneListBeginChecked(GPU_LIST_OP_POLY) < 0) goto pvr_error;
        SceneListSubmitFinal(
            scene->opaque.records, scene->opaque.record_count);
        if(SceneListFinishChecked() < 0) goto pvr_error;
        ++lists;
        records += (GLuint)scene->opaque.record_count;
    }
    if(scene->punch_through.record_count > 0) {
        if(SceneListBeginChecked(GPU_LIST_PT_POLY) < 0) goto pvr_error;
        SceneListSubmitFinal(
            scene->punch_through.records,
            scene->punch_through.record_count);
        if(SceneListFinishChecked() < 0) goto pvr_error;
        ++lists;
        records += (GLuint)scene->punch_through.record_count;
    }
    if(scene->translucent.record_count > 0) {
        if(SceneListBeginChecked(GPU_LIST_TR_POLY) < 0) goto pvr_error;
        SceneListSubmitFinal(
            scene->translucent.records,
            scene->translucent.record_count);
        if(SceneListFinishChecked() < 0) goto pvr_error;
        ++lists;
        records += (GLuint)scene->translucent.record_count;
    }
    {
        const int finish_result = SceneFinishChecked();
        scene_started = GL_FALSE;
        if(finish_result < 0) goto pvr_error;
    }

    GLDC_STAT_INC(pvr_exclusive_scenes);
    GLDC_STAT_ADD(pvr_exclusive_lists, lists);
    GLDC_STAT_ADD(pvr_exclusive_records, records);
    finish_exclusive_state(GL_TRUE);
    return GL_KOS_PVR_OK;

pvr_error:
    /* A failed list begin/finish still leaves a begun scene to close. Match
       KOS's checked raw-packet pattern: attempt scene finish, then invalidate
       every GLdc shadow before the caller can fall back or retry. */
    if(scene_started) (void)SceneFinishChecked();
    finish_exclusive_state(GL_FALSE);
    GLDC_STAT_INC(pvr_exclusive_rejects);
    return GL_KOS_PVR_PVR_ERROR;
#endif
}

#if defined(GLDC_NATIVE_BENCH) && GLDC_NATIVE_BENCH
/* Submit the ordinary GLdc queues from an already-ready PVR boundary.  The
   benchmark deliberately keeps pvr_wait_ready() outside its timer; everything
   from pvr_scene_begin through pvr_scene_finish remains inside, matching the
   exclusive raw and N1 envelopes.  N0 is opaque-only so accepting any other
   list/sprite traffic would make the comparison ill-defined. */
GLint APIENTRY glKosNativeBenchSubmitQueuedReady(void) {
#if GLDC_S3_SEGMENTED_OP
    if(_glS3SceneOpen()) {
        return GL_KOS_NATIVE_BENCH_UNSUPPORTED_STATE;
    }
#endif
    if(!list_has_content(&OP_LIST) ||
       list_has_content(&PT_LIST) || list_has_content(&TR_LIST) ||
       aligned_vector_size(&OP_LIST.sprites) != 0) {
        return GL_KOS_NATIVE_BENCH_UNSUPPORTED_STATE;
    }

    pvr_scene_begin();
    SceneListBegin(GPU_LIST_OP_POLY);
    submit_list(&OP_LIST);
    SceneListFinish();
    SceneFinish();
    return GL_KOS_NATIVE_BENCH_OK;
}

/* Queue lifetime is intentionally separated from submission so cleanup does
   not pollute the timed envelope.  This is benchmark-only and never services
   deferred texture frees: the isolated executable loads no game assets. */
void APIENTRY glKosNativeBenchResetQueued(void) {
    clear_lists();
#if GLDC_S3_SEGMENTED_OP
    _glS3SwapReset();
#endif
    _glInvalidateCapturedArrays();
}
#endif

#if GLDC_S3_SEGMENTED_OP
/* S3 swap-side OP: the tail of the vector (undrained remainder) plus the sprite
   sidecar, into the STILL-OPEN OP list. */
static void submit_op_tail_s3(void) {
    _glS3SubmitOpTail();
    const uint32_t sn = aligned_vector_size(&OP_LIST.sprites);
    if(sn) {
        SceneSpritesSubmit(aligned_vector_front(&OP_LIST.sprites), (int) sn);
    }
}
#endif

void APIENTRY glKosSwapBuffers() {
    TRACE();

#if GLDC_S3_SEGMENTED_OP
    if(_glS3SceneOpen()) {
        /* The scene began at the first mid-frame drain (its pvr_wait_ready is
           inside the s3= bucket); wait= reads ~0 here by design. Submit the OP
           tail + sprites and close the list the drains held open. */
        GT_MARK(_gt_op_us, submit_op_tail_s3());
        SceneListFinish();
    } else {
        /* No drain ran this frame (no OP->non-OP transition with content, or
           an OP-less frame): byte-identical legacy behavior. */
        GT_MARK(_gt_wait_us, SceneBegin());
        if(list_has_content(&OP_LIST)) {
            SceneListBegin(GPU_LIST_OP_POLY);
            GT_MARK(_gt_op_us, submit_list(&OP_LIST));
            SceneListFinish();
        }
    }
#else
    /* In the SQ build, wait= retains SceneBegin's previous-frame TA fence.
       In N4 it is list-budgeting plus only the exceptional growth/fog fence;
       the ordinary readiness wait moves to fin= where KOS launches DMA. */
#if GLDC_N4_VERTEX_DMA
    GT_MARK(_gt_wait_us, n4_begin_queued_scene());
#else
    GT_MARK(_gt_wait_us, SceneBegin());
#endif

    if(list_has_content(&OP_LIST)) {
        SceneListBegin(GPU_LIST_OP_POLY);
        GT_MARK(_gt_op_us, submit_list(&OP_LIST));
        SceneListFinish();
    }
#endif

#if GLDC_SWAP_TELEMETRY
    _gt_op_verts += aligned_vector_size(&OP_LIST.vector);
#ifdef _arch_dreamcast
    /* Replace each physical sentinel with the logical vertices transformed at
       swap so OP ns/v and the reported TR volume keep honest denominators. */
    _gt_op_verts += _glDeferredP3T2BGRAListVertexCount(&OP_LIST) -
                    _glDeferredP3T2BGRAListCount(&OP_LIST);
    _gt_tr_verts += _glDeferredP3T2BGRAListVertexCount(&TR_LIST) -
                    _glDeferredP3T2BGRAListCount(&TR_LIST);
    _gt_op_verts += _glPvrPacketListRecordCount(&OP_LIST) -
                    _glPvrPacketListCount(&OP_LIST);
    _gt_tr_verts += _glPvrPacketListRecordCount(&TR_LIST) -
                    _glPvrPacketListCount(&TR_LIST);
#endif
    _gt_tr_verts += aligned_vector_size(&TR_LIST.vector);
#endif

    if(list_has_content(&PT_LIST)) {
        SceneListBegin(GPU_LIST_PT_POLY);
        GT_MARK(_gt_pt_us, submit_list(&PT_LIST));
        SceneListFinish();
    }

    if(list_has_content(&TR_LIST)) {
        SceneListBegin(GPU_LIST_TR_POLY);
        GT_MARK(_gt_tr_us, submit_list(&TR_LIST));
        SceneListFinish();
    }

    GT_MARK(_gt_fin_us, SceneFinish());

#if GLDC_SWAP_TELEMETRY
    _gt_frames++;
#endif  /* GLDC_SWAP_TELEMETRY */

    clear_lists();
#if GLDC_S3_SEGMENTED_OP
    _glS3SwapReset();
#endif

    _glApplyScissor(true);

    _glProcessDeferredFrees();   /* release texture VRAM queued >= 2 swaps ago */
    _glInvalidateCapturedArrays();
}

/* Render everything submitted so far into a VRAM texture instead of the screen,
   then clear the lists. This is pass 1 of the two-pass HUD overlay: the caller
   renders the world, calls this to bake it into `tex`, then draws `tex` as a
   full-screen quad plus the HUD and ends the frame normally (glKosSwapBuffers) —
   so the OP/PT HUD composites on top of the already-flattened world (including
   all its TR). `tex` must be a pvr_mem_malloc'd target of (w x h), power-of-two. */
void APIENTRY glKosFlushToTexture(void* tex, unsigned int w, unsigned int h) {
    TRACE();

#if GLDC_S3_SEGMENTED_OP
    /* S3 opens the to-screen scene at the first mid-frame drain; a mid-frame
       render-to-texture pass cannot retarget it. Unused by HyperSolar — fail
       loudly instead of corrupting the TA. */
    if(_glS3SceneOpen()) {
        fprintf(stderr, "glKosFlushToTexture: unsupported mid-frame with GLDC_S3_SEGMENTED_OP (scene already open)\n");
        return;
    }
#endif

#if GLDC_N4_VERTEX_DMA
    n4_begin_queued_scene_to_texture(tex, w, h);
#else
    SceneBeginToTexture(tex, w, h);
#endif
        if(list_has_content(&OP_LIST)) {
            SceneListBegin(GPU_LIST_OP_POLY);
            submit_list(&OP_LIST);
            SceneListFinish();
        }

        if(list_has_content(&PT_LIST)) {
            SceneListBegin(GPU_LIST_PT_POLY);
            submit_list(&PT_LIST);
            SceneListFinish();
        }

        if(list_has_content(&TR_LIST)) {
            SceneListBegin(GPU_LIST_TR_POLY);
            submit_list(&TR_LIST);
            SceneListFinish();
        }
    SceneFinish();

    clear_lists();

    _glApplyScissor(true);

    /* Captures index into the vectors just cleared — a replay between here and
       the next swap would copy recycled memory into the TA. */
    _glInvalidateCapturedArrays();
}
