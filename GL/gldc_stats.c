/*
 * gldc_stats.c - GLdc performance instrumentation implementation
 *
 * Compile with -DGLDC_ENABLE_STATS to activate counters.
 */

#include <stdio.h>
#include <string.h>
#include "gldc_stats.h"

#ifdef GLDC_ENABLE_STATS

GLdcStats g_gldc_stats = {
    .struct_size = sizeof(GLdcStats),
    .abi_version = GL_KOS_STATS_ABI_VERSION
};

void APIENTRY glKosResetStats(void) {
    const GLuint frame = g_gldc_stats.frame_no + 1;
    memset(&g_gldc_stats, 0, sizeof(GLdcStats));
    g_gldc_stats.struct_size = sizeof(GLdcStats);
    g_gldc_stats.abi_version = GL_KOS_STATS_ABI_VERSION;
    g_gldc_stats.frame_no = frame;
}

const GLdcStats* APIENTRY glKosGetStats(void) {
    return &g_gldc_stats;
}

void APIENTRY glKosPrintStats(void) {
    const GLdcStats* s = &g_gldc_stats;

    /* Compute derived metrics */
    GLuint avg_vtx_per_draw = s->submit_vertices_calls > 0
        ? s->vertices_transformed / s->submit_vertices_calls : 0;
    GLuint avg_strip_len = s->strip_count > 0
        ? s->strip_vertices_total / s->strip_count : 0;

    /* Line 1: Draw submission (original Patch B) */
    printf("[GLdc F#%u] arr=%u elem=%u submit=%u attribFast=%u attribSlow=%u hdr=%u "
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

    /* Line 2: Triangles classified inside the generic clip fallback only. */
    printf("[GLdc F#%u] gclip: tested=%u all=%u none=%u partial=%u edges=%u\n",
           s->frame_no,
           s->clip_triangles_tested,
           s->clip_all_visible,
           s->clip_none_visible,
           s->clip_partial,
           s->clip_edges_generated);

    /* Line 3: Scene submission. `generic` is the post-clip output count. */
    printf("[GLdc F#%u] scene: submits=%u records_in=%u hdrs=%u divided=%u "
           "generic=%u sprites=%u offset=%u\n",
           s->frame_no,
           s->scene_list_submits,
           s->scene_records_in,
           s->scene_headers_seen,
           s->scene_divided_records,
           s->scene_generic_records,
           s->scene_sprite_records,
           s->polygon_offset_vertices);

    /* Line 4: Fast-lane routing; pairs are accepted/fallback calls. */
    if (s->multistrip_hits > 0 || s->multistrip_fallbacks > 0 ||
        s->triangle_array_hits > 0 || s->triangle_array_fallbacks > 0 ||
        s->planar_quad_hits > 0 || s->planar_quad_fallbacks > 0 ||
        s->quad_strip_hits > 0 || s->quad_strip_fallbacks > 0 ||
        s->sprite_lane_hits > 0 || s->sprite_lane_drops > 0 ||
        s->interleaved_hits > 0 || s->interleaved_fallbacks > 0) {
        printf("[GLdc F#%u] lanes: multi=%u/%u strips=%u avgLen=%u "
               "tri=%u/%u planar=%u/%u qstrip=%u/%u sprite=%u/%u items=%u "
               "p3t2=%u/%u verts=%u\n",
               s->frame_no,
               s->multistrip_hits,
               s->multistrip_fallbacks,
               s->strip_count,
               avg_strip_len,
               s->triangle_array_hits,
               s->triangle_array_fallbacks,
               s->planar_quad_hits,
               s->planar_quad_fallbacks,
               s->quad_strip_hits,
               s->quad_strip_fallbacks,
               s->sprite_lane_hits,
               s->sprite_lane_drops,
               s->sprite_items,
               s->interleaved_hits,
               s->interleaved_fallbacks,
               s->interleaved_vertices);
        if(s->interleaved_fallbacks > 0) {
            printf("[GLdc F#%u] p3t2 fall: mode/count=%u align=%u tnl=%u "
                   "immediate=%u radial=%u\n",
                   s->frame_no,
                   s->interleaved_fallback_mode_or_count,
                   s->interleaved_fallback_alignment,
                   s->interleaved_fallback_tnl,
                   s->interleaved_fallback_immediate,
                   s->interleaved_fallback_radial_fog);
        }
    }

    if(s->deferred_quad_attempts > 0 ||
       s->deferred_descriptors_submitted > 0) {
        printf("[GLdc F#%u] n2: try=%u hit=%u fall=%u queued=%u "
               "drain=%u direct=%u nearq=%u soa=%u/%u/%u/%u:%u/%u/%u "
               "mstrip=%u/%u/%u/%u/%u:%u/%u/%u "
               "tri=%u/%u/%u/%u:%u/%u/%u "
               "color=%u/%u/%u/%u:%u/%u/%u "
               "reject=%u/%u/%u/%u/%u/%u\n",
               s->frame_no,
               s->deferred_quad_attempts,
               s->deferred_quad_hits,
               s->deferred_quad_fallbacks,
               s->deferred_quad_vertices,
               s->deferred_descriptors_submitted,
               s->deferred_direct_vertices,
               s->deferred_near_quads,
               s->deferred_array_attempts,
               s->deferred_array_hits,
               s->deferred_array_fallbacks,
               s->deferred_array_vertices,
               s->deferred_array_descriptors_submitted,
               s->deferred_array_direct_vertices,
               s->deferred_array_near_quads,
               s->deferred_multistrip_attempts,
               s->deferred_multistrip_hits,
               s->deferred_multistrip_fallbacks,
               s->deferred_multistrip_strips,
               s->deferred_multistrip_vertices,
               s->deferred_multistrip_descriptors_submitted,
               s->deferred_multistrip_direct_vertices,
               s->deferred_multistrip_near_fallbacks,
               s->deferred_triangle_attempts,
               s->deferred_triangle_hits,
               s->deferred_triangle_fallbacks,
               s->deferred_triangle_vertices,
               s->deferred_triangle_descriptors_submitted,
               s->deferred_triangle_direct_vertices,
               s->deferred_triangle_near_fallbacks,
               s->deferred_color_array_attempts,
               s->deferred_color_array_hits,
               s->deferred_color_array_fallbacks,
               s->deferred_color_array_vertices,
               s->deferred_color_array_descriptors_submitted,
               s->deferred_color_array_direct_vertices,
               s->deferred_color_array_near_quads,
               s->deferred_reject_disabled,
               s->deferred_reject_mode_or_count,
               s->deferred_reject_alignment,
               s->deferred_reject_state,
               s->deferred_reject_capture,
               s->deferred_reject_capacity);
    }

    /* Line 5: N3 final-packet traffic (if any). */
    if(s->pvr_packet_reserve_attempts > 0 ||
       s->pvr_packet_segments_submitted > 0 ||
       s->pvr_typed_attempts > 0 ||
       s->pvr_exclusive_scenes > 0 ||
       s->pvr_exclusive_rejects > 0) {
        printf("[GLdc F#%u] n3: reserve=%u/%u commit=%u cancel=%u v=%u "
               "drain=%u/%u typed=%u/%u/%u near=%u "
               "reject=%u/%u/%u/%u exclusive=%u/%u/%u/%u\n",
               s->frame_no,
               s->pvr_packet_reserve_hits,
               s->pvr_packet_reserve_attempts,
               s->pvr_packet_commits,
               s->pvr_packet_cancels,
               s->pvr_packet_vertices,
               s->pvr_packet_segments_submitted,
               s->pvr_packet_records_submitted,
               s->pvr_typed_attempts,
               s->pvr_typed_hits,
               s->pvr_typed_fallbacks,
               s->pvr_typed_near_fallbacks,
               s->pvr_packet_reject_busy,
               s->pvr_packet_reject_state,
               s->pvr_packet_reject_capacity,
               s->pvr_packet_reject_validation,
               s->pvr_exclusive_scenes,
               s->pvr_exclusive_lists,
               s->pvr_exclusive_records,
               s->pvr_exclusive_rejects);
    }

    /* Line 6: Immediate mode (should be near zero with the batcher). */
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
void APIENTRY glKosResetStats(void) {}
const GLdcStats* APIENTRY glKosGetStats(void) { return (const GLdcStats*)0; }
void APIENTRY glKosPrintStats(void) {}

#endif /* GLDC_ENABLE_STATS */
