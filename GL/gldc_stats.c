/*
 * gldc_stats.c - GLdc Performance Instrumentation Implementation (Phase 0 expanded)
 *
 * Compile with -DGLDC_ENABLE_STATS to activate counters.
 */

#include <stdio.h>
#include <string.h>
#include "gldc_stats.h"

#ifdef GLDC_ENABLE_STATS

GLdcStats g_gldc_stats = {0};

void glKosResetStats(void) {
    uint32_t frame = g_gldc_stats.frame_no + 1;
    memset(&g_gldc_stats, 0, sizeof(GLdcStats));
    g_gldc_stats.frame_no = frame;
}

const GLdcStats* glKosGetStats(void) {
    return &g_gldc_stats;
}

void glKosPrintStats(void) {
    const GLdcStats* s = &g_gldc_stats;

    /* Compute derived metrics */
    uint32_t avg_vtx_per_draw = s->submit_vertices_calls > 0
        ? s->vertices_transformed / s->submit_vertices_calls : 0;
    uint32_t avg_strip_len = s->strip_count > 0
        ? s->strip_vertices_total / s->strip_count : 0;

    /* Line 1: Draw submission (original Patch B) */
    printf("[GLdc F#%u] arr=%u elem=%u submit=%u fast=%u miss=%u hdr=%u "
           "dirty=%u vtx=%u tex=%u avgVtx=%u\n",
           s->frame_no,
           s->draw_arrays_calls,
           s->draw_elements_calls,
           s->submit_vertices_calls,
           s->fast_path_hits,
           s->fast_path_misses,
           s->headers_emitted,
           s->state_dirty_events,
           s->vertices_transformed,
           s->texture_binds,
           avg_vtx_per_draw);

    /* Line 2: Clipping */
    printf("[GLdc F#%u] clip: tested=%u all=%u none=%u partial=%u edges=%u\n",
           s->frame_no,
           s->clip_triangles_tested,
           s->clip_all_visible,
           s->clip_none_visible,
           s->clip_partial,
           s->clip_edges_generated);

    /* Line 3: Scene submission */
    printf("[GLdc F#%u] scene: submits=%u verts_in=%u hdrs=%u\n",
           s->frame_no,
           s->scene_list_submits,
           s->scene_vertices_in,
           s->scene_headers_seen);

    /* Line 4: Strips + Patch E (future phases, prints zeros until active) */
    if (s->strip_count > 0 || s->patchE_hits > 0 || s->patchE_fallbacks > 0) {
        printf("[GLdc F#%u] strips=%u avgLen=%u patchE: hits=%u fall=%u\n",
               s->frame_no,
               s->strip_count,
               avg_strip_len,
               s->patchE_hits,
               s->patchE_fallbacks);
    }

    /* Line 5: Immediate mode (if any — should be near zero with batcher) */
    if (s->immediate_begin_calls > 0) {
        printf("[GLdc F#%u] imm: begin=%u end=%u vtx=%u\n",
               s->frame_no,
               s->immediate_begin_calls,
               s->immediate_end_calls,
               s->immediate_vertices);
    }
}

#else

/* Stubs when stats are disabled */
void glKosResetStats(void) {}
const void* glKosGetStats(void) { return (const void*)0; }
void glKosPrintStats(void) {}

#endif /* GLDC_ENABLE_STATS */
