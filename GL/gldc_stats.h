/*
 * gldc_stats.h - GLdc Performance Instrumentation Counters
 *
 * Patch B: Add this file to GLdc/GL/ and include it from draw.c and state.c.
 * Provides per-frame counters for draw calls, fast-path hits/misses,
 * poly headers emitted, state dirty events, vertex transforms, and texture binds.
 *
 * Usage in game code:
 *   extern void glKosResetStats(void);
 *   extern const GLdcStats* glKosGetStats(void);
 *   extern void glKosPrintStats(void);
 *
 *   // Each frame, after EndDrawing():
 *   glKosPrintStats();
 *   glKosResetStats();
 */

#ifndef GLDC_STATS_H
#define GLDC_STATS_H

#include <stdint.h>

/* Define GLDC_ENABLE_STATS to compile with instrumentation.
 * When not defined, all stat macros become no-ops for zero overhead. */

typedef struct {
    uint32_t frame_no;
    uint32_t draw_arrays_calls;
    uint32_t draw_elements_calls;
    uint32_t submit_vertices_calls;
    uint32_t fast_path_hits;
    uint32_t fast_path_misses;
    uint32_t headers_emitted;
    uint32_t state_dirty_events;
    uint32_t vertices_transformed;
    uint32_t texture_binds;
    uint32_t immediate_begin_calls;
    uint32_t immediate_end_calls;
    uint32_t immediate_vertices;
} GLdcStats;

#ifdef GLDC_ENABLE_STATS

extern GLdcStats g_gldc_stats;

#define GLDC_STAT_INC(field)         (g_gldc_stats.field++)
#define GLDC_STAT_ADD(field, n)      (g_gldc_stats.field += (n))

#else

#define GLDC_STAT_INC(field)         ((void)0)
#define GLDC_STAT_ADD(field, n)      ((void)0)

#endif /* GLDC_ENABLE_STATS */

#endif /* GLDC_STATS_H */
