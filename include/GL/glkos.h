#pragma once

#include "gl.h"

__BEGIN_DECLS

extern const char* GLDC_VERSION;

/* Paired fast-lane ABI shared by GLdc and tightly coupled consumers such as
 * raylib-dc. Bump this whenever a glKos fast-path signature or data contract
 * changes incompatibly; it is deliberately independent of GLDC_VERSION. */
#define GL_KOS_FAST_PATH_ABI_VERSION 1u
#define GL_KOS_HAS_INTERLEAVED_P3T2BGRA 1
#define GL_KOS_FAST_PATH_INTERLEAVED_P3T2BGRA (1u << 0)
#define GL_KOS_FAST_PATH_CAPABILITIES GL_KOS_FAST_PATH_INTERLEAVED_P3T2BGRA

/* Borrowed, synchronous fast-lane input shared with paired adapters such as
 * raylib-dc. The implementation consumes/copies every vertex before return;
 * this type does NOT imply swap-stable lifetime or deferred submission. */
typedef struct GLKosVertexP3T2BGRA {
    GLfloat x, y, z;
    GLfloat u, v;
    GLuint bgra;
} GLKosVertexP3T2BGRA;

/* Compile-time feature macros let an independently built adapter retain its
 * ordinary client-array fallback when paired with an older GLdc header. */
GLAPI GLuint APIENTRY glKosGetFastPathCapabilities(void);

/* Try the fixed interleaved P3F/T2F/BGRA lane without changing client-array
 * state. GL_TRUE means the draw was consumed synchronously. GL_FALSE means no
 * render/list/header/capture state changed and the caller must use its exact
 * ordinary fallback (instrumentation counters may change). V1 accepts only
 * aligned complete GL_TRIANGLES/GL_QUADS batches, no active TnL effects or
 * glBegin/glEnd, and radial fog OFF or BLEND_PRECOMPUTED. */
GLAPI GLboolean APIENTRY glKosTryDrawInterleavedP3T2BGRA(
    GLenum mode, const GLKosVertexP3T2BGRA* vertices, GLsizei count);

/* Link-time configuration canary. Every Dreamcast consumer should call the
 * macro once: a normal object can then never silently link a benchmark GLdc
 * archive (or vice versa). Exactly one of the suffixed symbols exists in a
 * given archive. */
GLAPI void APIENTRY glKosRequireNativeBenchArchive0(void);
GLAPI void APIENTRY glKosRequireNativeBenchArchive1(void);
#if defined(GLDC_NATIVE_BENCH) && GLDC_NATIVE_BENCH
#define glKosRequireNativeBenchArchive() glKosRequireNativeBenchArchive1()
#else
#define glKosRequireNativeBenchArchive() glKosRequireNativeBenchArchive0()
#endif

/* N0/N1 hardware-lab ABI. This is deliberately absent from ordinary builds:
 * it exposes final 32-byte TA records solely so an exclusive microbenchmark
 * can compare raw PVR submission, a fused object-to-record kernel, the normal
 * GLdc lane and raylib on identical input. It is not a game rendering API and
 * must never be mixed with a live GLdc scene/list. */
#if defined(GLDC_NATIVE_BENCH) && GLDC_NATIVE_BENCH
#define GL_KOS_NATIVE_BENCH_ABI_VERSION 2u

typedef struct __attribute__((aligned(32))) GLKosNativeBenchRecord {
    GLuint word[8];
} GLKosNativeBenchRecord;

enum {
    GL_KOS_NATIVE_BENCH_OK = 0,
    GL_KOS_NATIVE_BENCH_BAD_ARGUMENT = 1,
    GL_KOS_NATIVE_BENCH_UNSUPPORTED_STATE = 2,
    GL_KOS_NATIVE_BENCH_NEAR_CLIP = 3,
    GL_KOS_NATIVE_BENCH_MISMATCH = 4,
    GL_KOS_NATIVE_BENCH_PVR_ERROR = 5,
    GL_KOS_NATIVE_BENCH_CAPACITY = 6
};

/* Runtime companion to the link-time canary, for logging and diagnostics. */
GLAPI GLuint APIENTRY glKosNativeBenchArchiveAbiVersion(void);

/* Compile the current GL state into the exact header used by GLdc. This does
 * not emit a header or mark GL state clean. */
GLAPI GLint APIENTRY glKosNativeBenchCompileHeader(
    GLKosNativeBenchRecord* header);

/* Fused object-space -> final-record experiment. V2 accepts GL_TRIANGLES and
 * GL_TRIANGLE_STRIP with neutral polygon offset, no TnL effects/capture, and
 * radial vertex fog off. Every input record is transformed even when one
 * crosses the near plane; NEAR_CLIP is returned after the complete output has
 * been classified. Such unclipped output must not be submitted. */
GLAPI GLint APIENTRY glKosNativeBenchBuildP3T2BGRA(
    GLenum mode, const GLKosVertexP3T2BGRA* vertices, GLsizei count,
    GLKosNativeBenchRecord* output);

/* Contiguous long-strip batch: counts partitions one vertex array and stamps
 * one EOL per partition. The matrix is loaded once for the whole batch. */
GLAPI GLint APIENTRY glKosNativeBenchBuildMultiStripsP3T2BGRA(
    const GLKosVertexP3T2BGRA* vertices,
    const GLsizei* counts, GLsizei strip_count,
    GLKosNativeBenchRecord* output);

/* Build both the N1 result and the classic transform+finalize result in RAM,
 * then compare all eight words per record. mismatch_word receives the first
 * differing word index, or count*8 on success. */
GLAPI GLint APIENTRY glKosNativeBenchValidateP3T2BGRA(
    GLenum mode, const GLKosVertexP3T2BGRA* vertices, GLsizei count,
    GLKosNativeBenchRecord* candidate,
    GLKosNativeBenchRecord* classic,
    GLuint* mismatch_word);

/* Untimed exact near-plane oracle for independent triangles. packet receives
 * one compiled opaque header followed by the production clipper's final TA
 * records, including its exact order, duplicates and EOL flags. Capacity and
 * packet_records are measured in 32-byte records and include the header. On
 * CAPACITY, packet_records reports the exact required size; the prefix that
 * fit remains valid but must not be submitted. Input and packet must not
 * overlap. */
GLAPI GLint APIENTRY glKosNativeBenchBuildTrianglePacketP3T2BGRA(
    const GLKosVertexP3T2BGRA* vertices, GLsizei count,
    GLKosNativeBenchRecord* packet, GLsizei packet_capacity,
    GLsizei* packet_records);

/* True object-space -> TA path used by N0 route 2. The immutable input must
 * have returned OK from the RAM validator first. The timed kernel still
 * classifies every transformed record and returns NEAR_CLIP after completing,
 * but it does not clip. These calls own one exclusive opaque scene and must be
 * preceded by pvr_wait_ready(). */
GLAPI GLint APIENTRY glKosNativeBenchSubmitP3T2BGRAAllVisible(
    GLenum mode, const GLKosVertexP3T2BGRA* vertices, GLsizei count);
GLAPI GLint APIENTRY glKosNativeBenchSubmitMultiStripsP3T2BGRAAllVisible(
    const GLKosVertexP3T2BGRA* vertices,
    const GLsizei* counts, GLsizei strip_count);

/* Submit one contiguous header+final-record packet in an exclusive opaque PVR
 * scene. packet_records includes the header. The caller must pvr_wait_ready()
 * before the timed call; no ordinary GLdc/raylib geometry may be pending. */
GLAPI GLint APIENTRY glKosNativeBenchSubmitFinalPacket(
    const GLKosNativeBenchRecord* packet, GLsizei packet_records);

/* Queue-only benchmark envelope. SubmitQueuedReady assumes PVR readiness and
 * an opaque-only queued scene; ResetQueued discards it after the timer. */
GLAPI GLint APIENTRY glKosNativeBenchSubmitQueuedReady(void);
GLAPI void APIENTRY glKosNativeBenchResetQueued(void);
#endif

/* GLdcStats is public even when instrumentation is compiled out so callers
 * never need private GLdc headers or hand-written declarations. The API
 * functions are always linkable; glKosGetStats() returns NULL when
 * GLDC_ENABLE_STATS is disabled. Append fields and bump this version when the
 * snapshot layout changes. */
#define GL_KOS_STATS_ABI_VERSION 1u

typedef struct {
    GLuint struct_size;
    GLuint abi_version;
    GLuint frame_no;

    /* General draw submission. */
    GLuint draw_arrays_calls;
    GLuint draw_elements_calls;
    GLuint submit_vertices_calls;
    GLuint fast_path_hits;       /* generic generator's attribute fast path */
    GLuint fast_path_misses;
    GLuint headers_emitted;
    GLuint state_dirty_events;
    GLuint vertices_transformed;
    GLuint texture_binds;
    GLuint immediate_begin_calls;
    GLuint immediate_end_calls;
    GLuint immediate_vertices;

    /* Triangle classifications inside the generic near-plane fallback only;
     * all-visible strips handled by the outer divided-run scanner are absent. */
    GLuint clip_triangles_tested;
    GLuint clip_all_visible;
    GLuint clip_none_visible;
    GLuint clip_partial;
    GLuint clip_edges_generated;

    /* Final TA submission. Record counts are actual 32-byte records written;
     * generic clipping may therefore emit more records than it consumed. */
    GLuint scene_list_submits;
    GLuint scene_records_in;      /* headers plus vertices entering finalizer */
    GLuint scene_headers_seen;
    GLuint scene_divided_records;
    GLuint scene_generic_records;
    GLuint scene_sprite_records;

    /* Records that paid the ordinary post-transform polygon-offset bake.
     * Direct TA sprites are intentionally excluded because they do not pay
     * that pass. */
    GLuint polygon_offset_vertices;

    /* glKos fast-lane routing. A hit means the specialized lane accepted the
     * draw; a fallback means it declined and routed to the ordinary GL path.
     * The latter may still be a no-op for invalid/degenerate GL input. */
    GLuint multistrip_hits;
    GLuint multistrip_fallbacks;
    GLuint strip_count;
    GLuint strip_vertices_total;
    GLuint triangle_array_hits;
    GLuint triangle_array_fallbacks;
    GLuint planar_quad_hits;
    GLuint planar_quad_fallbacks;
    GLuint quad_strip_hits;
    GLuint quad_strip_fallbacks;
    GLuint sprite_lane_hits;
    GLuint sprite_lane_drops;
    GLuint sprite_items;

    /* Borrowed interleaved P3F/T2F/BGRA routing. A rejected try mutates only
     * these optional counters; render state and pending capture are untouched. */
    GLuint interleaved_hits;
    GLuint interleaved_fallbacks;
    GLuint interleaved_vertices;
    GLuint interleaved_fallback_mode_or_count;
    GLuint interleaved_fallback_alignment;
    GLuint interleaved_fallback_tnl;
    GLuint interleaved_fallback_immediate;
    GLuint interleaved_fallback_radial_fog;
} GLdcStats;

GLAPI void APIENTRY glKosResetStats(void);
GLAPI const GLdcStats* APIENTRY glKosGetStats(void);
GLAPI void APIENTRY glKosPrintStats(void);


/*
 * Dreamcast specific compressed + twiddled formats.
 * We use constants from the range 0xEEE0 onwards
 * to avoid trampling any real GL constants (this is in the middle of the
 * any_vendor_future_use range defined in the GL enum.spec file.
*/
#define GL_UNSIGNED_SHORT_5_6_5_TWID_KOS            0xEEE0
#define GL_UNSIGNED_SHORT_1_5_5_5_REV_TWID_KOS      0xEEE2
#define GL_UNSIGNED_SHORT_4_4_4_4_REV_TWID_KOS      0xEEE3

#define GL_COMPRESSED_RGB_565_VQ_KOS                0xEEE4
#define GL_COMPRESSED_ARGB_1555_VQ_KOS              0xEEE6
#define GL_COMPRESSED_ARGB_4444_VQ_KOS              0xEEE7

#define GL_COMPRESSED_RGB_565_VQ_TWID_KOS           0xEEE8
#define GL_COMPRESSED_ARGB_1555_VQ_TWID_KOS         0xEEEA
#define GL_COMPRESSED_ARGB_4444_VQ_TWID_KOS         0xEEEB

#define GL_COMPRESSED_RGB_565_VQ_MIPMAP_KOS                0xEEEC
#define GL_COMPRESSED_ARGB_1555_VQ_MIPMAP_KOS              0xEEED
#define GL_COMPRESSED_ARGB_4444_VQ_MIPMAP_KOS              0xEEEE

#define GL_COMPRESSED_RGB_565_VQ_MIPMAP_TWID_KOS           0xEEEF
#define GL_COMPRESSED_ARGB_1555_VQ_MIPMAP_TWID_KOS         0xEEF0
#define GL_COMPRESSED_ARGB_4444_VQ_MIPMAP_TWID_KOS         0xEEF1

#define GL_NEARZ_CLIPPING_KOS                       0xEEFA


/* Initialize the GL pipeline. GL will initialize the PVR. */
GLAPI void APIENTRY glKosInit();

typedef struct {
    /* If GL_TRUE, enables pvr autosorting, this *will* break glDepthFunc/glDepthTest */
    GLboolean autosort_enabled;

    /* If GL_TRUE, enables the PVR FSAA */
    GLboolean fsaa_enabled;

    /* The internal format for paletted textures, must be GL_RGBA4 (default) or GL_RGBA8 */
    GLenum internal_palette_format;

    /* Initial capacity of each of the OP, TR and PT lists in vertices */
    GLuint initial_op_capacity;
    GLuint initial_tr_capacity;
    GLuint initial_pt_capacity;
    GLuint initial_immediate_capacity;

    /* Default: True
     *
     * Whether glTexImage should automatically twiddle textures
     * if the internal format is a generic format (e.g. GL_RGB).
     * this is the same as calling glEnable(GL_TEXTURE_TWIDDLE_KOS)
     * on boot */
    GLboolean texture_twiddle;
} GLdcConfig;


typedef struct {
    GLuint padding0;
    GLfloat x;
    GLfloat y;
    GLfloat z;
    GLfloat u;
    GLfloat v;
    GLubyte bgra[4];
    GLuint padding1;
} GLVertexKOS;

GLAPI void APIENTRY glVertexPackColor3fKOS(GLVertexKOS* vertex, float r, float g, float b);

/* Transform-once dual-list emit (fork extension, 2026-07-15): glKosCaptureArrays(slot) arms
   a capture — the NEXT draw call records the span it wrote into its poly list (post-TnL).
   glKosReplayArrays(slot, bgra) clones that span into the list selected by the CURRENT GPU
   state (texture/blend/fog header), overriding every vertex color with the constant `bgra`
   (4 bytes, GLdc vertex order) — or keeping the captured colors when NULL — and re-baking
   the current polygon-offset. Captures are invalidated at every swap, at glKosFlushToTexture,
   and at shutdown. PRECONDITION: the captured draw itself must run with polygon offset at
   identity — the replay re-bake assumes the capture is offset-free (it would compound).
   Intended for coplanar two-pass techniques that submit identical geometry twice. */
GLAPI void APIENTRY glKosCaptureArrays(GLuint slot);
/* Fused client-array lanes: one matrix load + one list extend for the whole
   batch, EOL prebaked (per strip / every 3rd vertex). Contract: vertex 3f /
   uv 2f / color 4ub arrays, any stride but colors 4-byte aligned; no
   ST/normals. GL lighting or non-identity texture/color matrices fall back to
   the general glDrawArrays path automatically. See draw.c. */
GLAPI void APIENTRY glKosDrawMultiStrips(const GLint* firsts, const GLsizei* counts, GLsizei n);
GLAPI void APIENTRY glKosDrawTrianglesArrays(GLint first, GLsizei count);
/* Dynamic planar-quad batches. Both entries consume the currently enabled
   P3F/T2F/BGRA client arrays and keep ordinary polygon-list clipping/order:

   - glKosDrawPlanarQuadsArrays skips exactly collapsed quads and derives each
     parallelogram's fourth clip-space corner from the first three. Input quads
     must satisfy object-space D=A+C-B.
   - glKosDrawQuadStripsArrays compacts a chain of adjacent input quads into one
     triangle strip (2*(quads+1) output vertices), removing shared endpoints.
     Face f vertices 0/3 and face f+1 vertices 1/2 must carry identical shared
     endpoint positions and colors. UVs must match too, except that a UV
     discontinuity is supported and starts a new strip at that endpoint.

   They fall back to ordinary GL_QUADS when TnL effects or an incompatible
   client layout is active. Counts are INPUT vertices and must be multiples of
   four. The return value is the actual polygon-record count submitted.
   Intended for dynamic holographic facade/cylinder streams. */
GLAPI GLsizei APIENTRY glKosDrawPlanarQuadsArrays(
    const GLint* firsts, const GLsizei* counts, GLsizei n);
GLAPI GLsizei APIENTRY glKosDrawQuadStripsArrays(
    const GLint* firsts, const GLsizei* counts, GLsizei n);
/* TA sprite quads: one 64-byte hardware sprite per planar single-color
   PARALLELOGRAM (D = A+C-B in object space)
   (vs four 32-byte vertices), color in a shared header emitted on change,
   transform+divide done at call time (bypasses the submit finalizer). pos =
   12 floats/quad in ring order, colors read at [quad*4] (the glow scratch
   layout, BGRA bytes = ARGB word). ADDITIVE/order-free content only (records
   land at the list tail); quads crossing the near plane are DROPPED whole
   (no sprite clip path). Draws nothing off Dreamcast. See draw.c. */
GLAPI void APIENTRY glKosDrawSpriteQuads(const GLfloat* pos, const GLuint* colors, GLsizei quads);
/* Homogeneous TA-sprite family: centers is 3 floats/sprite, colors is one
   packed word/sprite, and u/v are the shared object-space half axes. This is
   the low-bandwidth glow path: transform each axis once and one center per
   sprite instead of three corners. Same additive/near-plane contract above. */
GLAPI void APIENTRY glKosDrawSpriteCenters(const GLfloat* centers, const GLuint* colors,
                                           GLsizei sprites,
                                           GLfloat ux, GLfloat uy, GLfloat uz,
                                           GLfloat vx, GLfloat vy, GLfloat vz);
/* Variable-size center lane: one half-size and (u0,v0,u1,v1) rect per sprite. */
GLAPI void APIENTRY glKosDrawSpriteCentersUVRectScale(
    const GLfloat* centers, const GLuint* colors,
    const GLfloat* half_sizes, const GLfloat* uv_rects, GLsizei sprites,
    GLfloat ux, GLfloat uy, GLfloat uz, GLfloat vx, GLfloat vy, GLfloat vz);
/* View-plane specialization of the variable-size center lane. The shared u/v
   axes MUST lie in the current camera plane (ordinary and rotated billboards
   both qualify). All four corners then share one homogeneous W, so the SH4
   path performs one reciprocal per sprite instead of four while retaining the
   same whole-sprite near-plane rejection and TA-sidecar ordering. */
GLAPI void APIENTRY glKosDrawSpriteCentersUVRectScalePlane(
    const GLfloat* centers, const GLuint* colors,
    const GLfloat* half_sizes, const GLfloat* uv_rects, GLsizei sprites,
    GLfloat ux, GLfloat uy, GLfloat uz, GLfloat vx, GLfloat vy, GLfloat vz);
#define GL_KOS_UV_CELL_MAX_GRID_LOG2 3
/* Compact square-atlas sibling (grid_log2=1/2/3 for 2x2/4x4/8x8). `cells`
   replaces four UV floats per sprite; inset is normalized texture space and
   is applied at every cell edge. */
GLAPI void APIENTRY glKosDrawSpriteCentersUVCellScalePlane(
    const GLfloat* centers, const GLuint* colors,
    const GLfloat* half_sizes, const GLubyte* cells, GLsizei sprites,
    GLint grid_log2, GLfloat inset,
    GLfloat ux, GLfloat uy, GLfloat uz, GLfloat vx, GLfloat vy, GLfloat vz);
GLAPI void APIENTRY glKosReplayArrays(GLuint slot, const GLubyte* bgra);
GLAPI void APIENTRY glVertexPackColor4fKOS(GLVertexKOS* vertex, float r, float g, float b, float a);

GLAPI void APIENTRY glKosInitConfig(GLdcConfig* config);

/* Usage:
 *
 * GLdcConfig config;
 * glKosInitConfig(&config);
 *
 * config.autosort_enabled = GL_TRUE;
 *
 * glKosInitEx(&config);
 */
GLAPI void APIENTRY glKosInitEx(GLdcConfig* config);
GLAPI void APIENTRY glKosSwapBuffers();

/* Queue PVR fog table/color register writes so they are applied after GLdc has
   waited for the renderer and immediately before the next scene begins. This
   avoids changing global fog registers while real hardware may still be drawing
   the previous scene. Color order matches KOS: a, r, g, b. */
GLAPI void APIENTRY glKosQueueFogTableLinear(GLfloat a, GLfloat r, GLfloat g, GLfloat b,
                                            GLfloat start, GLfloat end);
GLAPI void APIENTRY glKosQueueFogTableFlat(GLfloat amount, GLfloat a, GLfloat r, GLfloat g,
                                          GLfloat b, GLfloat farDepth);
GLAPI void APIENTRY glKosQueueFogTableExp2(GLfloat a, GLfloat r, GLfloat g, GLfloat b,
                                          GLfloat start, GLfloat end, GLfloat power);
/* Queue the global PVR vertex-fog color for the next scene. The register is
   global (unlike a polygon header), so it is latched only at SceneBegin. */
GLAPI void APIENTRY glKosQueueFogVertexColor(GLfloat r, GLfloat g, GLfloat b);

/* Render everything submitted so far into a VRAM texture (`tex`, a
   pvr_mem_malloc'd w x h power-of-two target) instead of the screen, then clear
   the lists. Pass 1 of the two-pass HUD overlay — see flush.c. */
GLAPI void APIENTRY glKosFlushToTexture(void* tex, unsigned int w, unsigned int h);

/* Raw VRAM pointer of a texture id's data, for use as a glKosFlushToTexture
   target (texture must be a NONTWIDDLED 16-bit format). NULL if no data. */
GLAPI GLvoid* APIENTRY glKosTextureData(GLuint texId);

GLAPI void APIENTRY glKosShutdown();

/*
 * CUSTOM EXTENSION multiple_shared_palette_KOS
 *
 * This extension allows using up to 4 different shared palettes
 * with ColorTableEXT. The following constants are provided
 * to use as targets for ColorTableExt:
 *
 * - SHARED_TEXTURE_PALETTE_0_KOS
 * - SHARED_TEXTURE_PALETTE_1_KOS
 * - SHARED_TEXTURE_PALETTE_2_KOS
 * - SHARED_TEXTURE_PALETTE_3_KOS
 *
 * In this use case SHARED_TEXTURE_PALETTE_0_KOS is interchangable with SHARED_TEXTURE_PALETTE_EXT
 * (both refer to the first shared palette).
 *
 * To select which palette a texture uses, a new pname is accepted by TexParameteri: SHARED_TEXTURE_BANK_KOS
 * by default textures use shared palette 0.
*/


#define GL_SHARED_TEXTURE_PALETTE_0_KOS             0xEEFC
#define GL_SHARED_TEXTURE_PALETTE_1_KOS             0xEEFD
#define GL_SHARED_TEXTURE_PALETTE_2_KOS             0xEEFE
#define GL_SHARED_TEXTURE_PALETTE_3_KOS             0xEEFF
#define GL_SHARED_TEXTURE_PALETTE_4_KOS             0xEF00
#define GL_SHARED_TEXTURE_PALETTE_5_KOS             0xEF01
#define GL_SHARED_TEXTURE_PALETTE_6_KOS             0xEF02
#define GL_SHARED_TEXTURE_PALETTE_7_KOS             0xEF03
#define GL_SHARED_TEXTURE_PALETTE_8_KOS             0xEF04
#define GL_SHARED_TEXTURE_PALETTE_9_KOS             0xEF05

#define GL_SHARED_TEXTURE_PALETTE_10_KOS             0xEF06
#define GL_SHARED_TEXTURE_PALETTE_11_KOS             0xEF07
#define GL_SHARED_TEXTURE_PALETTE_12_KOS             0xEF08
#define GL_SHARED_TEXTURE_PALETTE_13_KOS             0xEF09
#define GL_SHARED_TEXTURE_PALETTE_14_KOS             0xEF0A
#define GL_SHARED_TEXTURE_PALETTE_15_KOS             0xEF0B
#define GL_SHARED_TEXTURE_PALETTE_16_KOS             0xEF0C
#define GL_SHARED_TEXTURE_PALETTE_17_KOS             0xEF0D
#define GL_SHARED_TEXTURE_PALETTE_18_KOS             0xEF0E
#define GL_SHARED_TEXTURE_PALETTE_19_KOS             0xEF0F

#define GL_SHARED_TEXTURE_PALETTE_20_KOS             0xEF10
#define GL_SHARED_TEXTURE_PALETTE_21_KOS             0xEF11
#define GL_SHARED_TEXTURE_PALETTE_22_KOS             0xEF12
#define GL_SHARED_TEXTURE_PALETTE_23_KOS             0xEF13
#define GL_SHARED_TEXTURE_PALETTE_24_KOS             0xEF14
#define GL_SHARED_TEXTURE_PALETTE_25_KOS             0xEF15
#define GL_SHARED_TEXTURE_PALETTE_26_KOS             0xEF16
#define GL_SHARED_TEXTURE_PALETTE_27_KOS             0xEF17
#define GL_SHARED_TEXTURE_PALETTE_28_KOS             0xEF18
#define GL_SHARED_TEXTURE_PALETTE_29_KOS             0xEF19

#define GL_SHARED_TEXTURE_PALETTE_30_KOS             0xEF1A
#define GL_SHARED_TEXTURE_PALETTE_31_KOS             0xEF1B
#define GL_SHARED_TEXTURE_PALETTE_32_KOS             0xEF1C
#define GL_SHARED_TEXTURE_PALETTE_33_KOS             0xEF1D
#define GL_SHARED_TEXTURE_PALETTE_34_KOS             0xEF1E
#define GL_SHARED_TEXTURE_PALETTE_35_KOS             0xEF1F
#define GL_SHARED_TEXTURE_PALETTE_36_KOS             0xEF20
#define GL_SHARED_TEXTURE_PALETTE_37_KOS             0xEF21
#define GL_SHARED_TEXTURE_PALETTE_38_KOS             0xEF22
#define GL_SHARED_TEXTURE_PALETTE_39_KOS             0xEF23

#define GL_SHARED_TEXTURE_PALETTE_40_KOS             0xEF24
#define GL_SHARED_TEXTURE_PALETTE_41_KOS             0xEF25
#define GL_SHARED_TEXTURE_PALETTE_42_KOS             0xEF26
#define GL_SHARED_TEXTURE_PALETTE_43_KOS             0xEF27
#define GL_SHARED_TEXTURE_PALETTE_44_KOS             0xEF28
#define GL_SHARED_TEXTURE_PALETTE_45_KOS             0xEF29
#define GL_SHARED_TEXTURE_PALETTE_46_KOS             0xEF2A
#define GL_SHARED_TEXTURE_PALETTE_47_KOS             0xEF2B
#define GL_SHARED_TEXTURE_PALETTE_48_KOS             0xEF2C
#define GL_SHARED_TEXTURE_PALETTE_49_KOS             0xEF2D

#define GL_SHARED_TEXTURE_PALETTE_50_KOS             0xEF2E
#define GL_SHARED_TEXTURE_PALETTE_51_KOS             0xEF2F
#define GL_SHARED_TEXTURE_PALETTE_52_KOS             0xEF30
#define GL_SHARED_TEXTURE_PALETTE_53_KOS             0xEF31
#define GL_SHARED_TEXTURE_PALETTE_54_KOS             0xEF32
#define GL_SHARED_TEXTURE_PALETTE_55_KOS             0xEF33
#define GL_SHARED_TEXTURE_PALETTE_56_KOS             0xEF34
#define GL_SHARED_TEXTURE_PALETTE_57_KOS             0xEF35
#define GL_SHARED_TEXTURE_PALETTE_58_KOS             0xEF36
#define GL_SHARED_TEXTURE_PALETTE_59_KOS             0xEF37

#define GL_SHARED_TEXTURE_PALETTE_60_KOS             0xEF38
#define GL_SHARED_TEXTURE_PALETTE_61_KOS             0xEF39
#define GL_SHARED_TEXTURE_PALETTE_62_KOS             0xEF3A
#define GL_SHARED_TEXTURE_PALETTE_63_KOS             0xEF3B

/* Pass to glTexParameteri to set the shared bank */
#define GL_SHARED_TEXTURE_BANK_KOS                  0xEF3C

/* Memory allocation extension (GL_KOS_texture_memory_management) */
GLAPI GLvoid APIENTRY glDefragmentTextureMemory_KOS(void);

/* Radial per-vertex fog fused into the existing client-array writer. Coordinates
   and radius are in the caller's current object space. BLEND selects the PVR's
   hardware vertex-fog combiner; BLEND_PRECOMPUTED selects the same combiner but
   consumes the coefficient already present in client color alpha, performing no
   position/radius work in GLdc. ATTENUATE_ALPHA only multiplies source alpha by
   (1-fog), for additive layers that must not add the fog color a second time.
   MIX_UNTEXTURED preserves source alpha while mixing vertex RGB; it is for sparse
   untextured translucent details whose alpha cannot also carry a fog coefficient. */
#define GL_KOS_VERTEX_FOG_OFF               0
#define GL_KOS_VERTEX_FOG_BLEND             1
#define GL_KOS_VERTEX_FOG_ATTENUATE_ALPHA   2
#define GL_KOS_VERTEX_FOG_MIX_UNTEXTURED    3
#define GL_KOS_VERTEX_FOG_BLEND_PRECOMPUTED 4
GLAPI GLvoid APIENTRY glKosVertexFogRadial(GLint mode,
                                           GLfloat center_x, GLfloat center_z,
                                           GLfloat radius, GLfloat alpha,
                                           GLfloat curve);

/* glGet extensions */
#define GL_FREE_TEXTURE_MEMORY_KOS                  0xEF3D
#define GL_USED_TEXTURE_MEMORY_KOS                  0xEF3E
#define GL_FREE_CONTIGUOUS_TEXTURE_MEMORY_KOS       0xEF3F

//for palette internal format (glfcConfig)
#define GL_RGB565_KOS                               0xEF40
#define GL_ARGB4444_KOS                             0xEF41
#define GL_ARGB1555_KOS                             0xEF42
#define GL_RGB565_TWID_KOS                          0xEF43
#define GL_ARGB4444_TWID_KOS                        0xEF44
#define GL_ARGB1555_TWID_KOS                        0xEF45
#define GL_COLOR_INDEX8_TWID_KOS                    0xEF46
#define GL_COLOR_INDEX4_TWID_KOS                    0xEF47
#define GL_RGB_TWID_KOS                             0xEF48
#define GL_RGBA_TWID_KOS                            0xEF49

/* glGet extensions */
#define GL_TEXTURE_INTERNAL_FORMAT_KOS              0xEF50

/* If enabled, will twiddle texture uploads where possible */
#define GL_TEXTURE_TWIDDLE_KOS                      0xEF51

__END_DECLS
