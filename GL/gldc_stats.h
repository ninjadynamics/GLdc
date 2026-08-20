/*
 * gldc_stats.h - internal GLdc instrumentation hooks
 *
 * Internal counter macros for the public GLdcStats snapshot in <GL/glkos.h>:
 *   - Draw call submission (Patch B original)
 *   - Near-plane clipping (Phase 0 new)
 *   - Final TA submission records
 *   - GLdc-owned fast-lane routing
 *
 * Consumers include <GL/glkos.h>; this private file only supplies zero-cost
 * producer hooks inside GLdc.
 */

#ifndef GLDC_STATS_H
#define GLDC_STATS_H

/* Use the source-tree public header even when a parent build prepends the
 * previously copied flat GLdc headers to KOS_CFLAGS. Those copies are updated
 * only after this library has built, so <GL/glkos.h> can be one build stale. */
#include "../include/GL/glkos.h"

/* Define GLDC_ENABLE_STATS to compile with instrumentation.
 * When not defined, all stat macros become no-ops for zero overhead. */

#ifdef GLDC_ENABLE_STATS

extern GLdcStats g_gldc_stats;

#define GLDC_STAT_INC(field)         (g_gldc_stats.field++)
#define GLDC_STAT_ADD(field, n)      (g_gldc_stats.field += (n))

#else

#define GLDC_STAT_INC(field)         ((void)0)
#define GLDC_STAT_ADD(field, n)      ((void)0)

#endif /* GLDC_ENABLE_STATS */

#endif /* GLDC_STATS_H */
