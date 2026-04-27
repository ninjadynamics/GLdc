/*
 * gldc_stats.c - GLdc Performance Instrumentation Implementation
 *
 * Patch B: Add this file to GLdc/GL/ and add it to the build.
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
    printf("[GLdc F#%u] arr=%u elem=%u submit=%u fast=%u miss=%u hdr=%u "
           "dirty=%u vtx=%u tex=%u imBegin=%u imEnd=%u imVtx=%u\n",
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
           s->immediate_begin_calls,
           s->immediate_end_calls,
           s->immediate_vertices);
}

#else

/* Stubs when stats are disabled */
void glKosResetStats(void) {}
const void* glKosGetStats(void) { return (const void*)0; }
void glKosPrintStats(void) {}

#endif /* GLDC_ENABLE_STATS */
