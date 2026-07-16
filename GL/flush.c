
#include "../containers/aligned_vector.h"
#include "private.h"

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

    printf("\nGLdc: [ CANARY ] Welcome to MODIFIED LOCAL GLdc! Git revision: %s [2026.07.15 23:49]\n", GLDC_VERSION);

#ifdef USE_SH4ZAM
    printf("GLdc: Hello SH4ZAM!\n\n");
#else
    printf("GLdc: SH4ZAM is NOT enabled.\n\n");
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
}

extern void _glInvalidateCapturedArrays(void);  /* draw.c: captures die with the cleared lists */
extern void _glResetDeferredFrees(void);        /* texture.c: drop queued records, no frees */

void APIENTRY glKosShutdown() {
    aligned_vector_clear(&OP_LIST.vector);
    aligned_vector_clear(&PT_LIST.vector);
    aligned_vector_clear(&TR_LIST.vector);

    _glInvalidateCapturedArrays();
    _glResetDeferredFrees();   /* ShutdownGPU tears the whole VRAM heap down anyway */

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
   aggregate print every 600 swaps; near-zero cost otherwise. */
#include <arch/timer.h>
#include <stdio.h>
static uint64_t _gt_wait_us, _gt_op_us, _gt_pt_us, _gt_tr_us, _gt_fin_us;
static int _gt_frames;
#define GT_MARK(var, expr) do { \
        uint64_t _t0 = timer_us_gettime64(); \
        expr; \
        var += timer_us_gettime64() - _t0; \
    } while(0)

void APIENTRY glKosSwapBuffers() {
    TRACE();

    /* NOTE: the "wait" bucket is SceneBegin = pvr_wait_ready + deferred fog-table
       apply + pvr_scene_begin, not the PVR fence alone. */
    GT_MARK(_gt_wait_us, SceneBegin());

    if(aligned_vector_header(&OP_LIST.vector)->size > 2) {
        SceneListBegin(GPU_LIST_OP_POLY);
        GT_MARK(_gt_op_us,
            SceneListSubmit((Vertex*) aligned_vector_front(&OP_LIST.vector), aligned_vector_size(&OP_LIST.vector)));
        SceneListFinish();
    }

    if(aligned_vector_header(&PT_LIST.vector)->size > 2) {
        SceneListBegin(GPU_LIST_PT_POLY);
        GT_MARK(_gt_pt_us,
            SceneListSubmit((Vertex*) aligned_vector_front(&PT_LIST.vector), aligned_vector_size(&PT_LIST.vector)));
        SceneListFinish();
    }

    if(aligned_vector_header(&TR_LIST.vector)->size > 2) {
        SceneListBegin(GPU_LIST_TR_POLY);
        GT_MARK(_gt_tr_us,
            SceneListSubmit((Vertex*) aligned_vector_front(&TR_LIST.vector), aligned_vector_size(&TR_LIST.vector)));
        SceneListFinish();
    }

    GT_MARK(_gt_fin_us, SceneFinish());

    if(++_gt_frames >= 600) {
        const float inv = 1.0f / (1000.0f * (float)_gt_frames);
        fprintf(stderr, "[GLDC-T] swap ms avg: wait=%.2f op=%.2f pt=%.2f tr=%.2f fin=%.2f (%d swaps)\n",
                (float)_gt_wait_us * inv, (float)_gt_op_us * inv, (float)_gt_pt_us * inv,
                (float)_gt_tr_us * inv, (float)_gt_fin_us * inv, _gt_frames);
        _gt_wait_us = _gt_op_us = _gt_pt_us = _gt_tr_us = _gt_fin_us = 0;
        _gt_frames = 0;
    }

    aligned_vector_clear(&OP_LIST.vector);
    aligned_vector_clear(&PT_LIST.vector);
    aligned_vector_clear(&TR_LIST.vector);

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

    SceneBeginToTexture(tex, w, h);
        if(aligned_vector_header(&OP_LIST.vector)->size > 2) {
            SceneListBegin(GPU_LIST_OP_POLY);
            SceneListSubmit((Vertex*) aligned_vector_front(&OP_LIST.vector), aligned_vector_size(&OP_LIST.vector));
            SceneListFinish();
        }

        if(aligned_vector_header(&PT_LIST.vector)->size > 2) {
            SceneListBegin(GPU_LIST_PT_POLY);
            SceneListSubmit((Vertex*) aligned_vector_front(&PT_LIST.vector), aligned_vector_size(&PT_LIST.vector));
            SceneListFinish();
        }

        if(aligned_vector_header(&TR_LIST.vector)->size > 2) {
            SceneListBegin(GPU_LIST_TR_POLY);
            SceneListSubmit((Vertex*) aligned_vector_front(&TR_LIST.vector), aligned_vector_size(&TR_LIST.vector));
            SceneListFinish();
        }
    SceneFinish();

    aligned_vector_clear(&OP_LIST.vector);
    aligned_vector_clear(&PT_LIST.vector);
    aligned_vector_clear(&TR_LIST.vector);

    _glApplyScissor(true);

    /* Captures index into the vectors just cleared — a replay between here and
       the next swap would copy recycled memory into the TA. */
    _glInvalidateCapturedArrays();
}
