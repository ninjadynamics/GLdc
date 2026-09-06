#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <stddef.h>

#include "private.h"
#include "platform.h"
#include "gldc_stats.h"

GLubyte ACTIVE_CLIENT_TEXTURE;

extern GLboolean AUTOSORT_ENABLED;

#define ITERATE(count) \
    GLuint i = count; \
    while(i--)


typedef GLuint (*IndexParseFunc)(const GLubyte* in);

static inline GLuint _parseUByteIndex(const GLubyte* in) {
    return (GLuint) *in;
}

static inline GLuint _parseUIntIndex(const GLubyte* in) {
    return *((GLuint*) in);
}

static inline GLuint _parseUShortIndex(const GLubyte* in) {
    /* UNSIGNED short: the old signed read sign-extended indices >= 32768
       into ~4G offsets (AUD-001-OPA-06). */
    return *((const GLushort*) in);
}

GL_FORCE_INLINE GLsizei index_size(GLenum type) {
    switch(type) {
    case GL_UNSIGNED_BYTE: return sizeof(GLubyte);
    case GL_UNSIGNED_SHORT: return sizeof(GLushort);
    case GL_UNSIGNED_INT: return sizeof(GLuint);
    default: return sizeof(GLushort);
    }
}

GL_FORCE_INLINE IndexParseFunc _calcParseIndexFunc(GLenum type) {
    switch(type) {
    case GL_UNSIGNED_BYTE:
        return &_parseUByteIndex;
    break;
    case GL_UNSIGNED_INT:
        return &_parseUIntIndex;
    break;
    case GL_UNSIGNED_SHORT:
    default:
        break;
    }

    return &_parseUShortIndex;
}


/* There was a bug in this macro that shipped with Kos
 * which has now been fixed. But just in case...
 */
#undef mat_trans_single3_nodiv
#define mat_trans_single3_nodiv(x, y, z) { \
    register float __x __asm__("fr12") = (x); \
    register float __y __asm__("fr13") = (y); \
    register float __z __asm__("fr14") = (z); \
    __asm__ __volatile__( \
                          "fldi1 fr15\n" \
                          "ftrv  xmtrx, fv12\n" \
                          : "=f" (__x), "=f" (__y), "=f" (__z) \
                          : "0" (__x), "1" (__y), "2" (__z) \
                          : "fr15"); \
    x = __x; y = __y; z = __z; \
}


/* FIXME: Is this right? Shouldn't it be fr12->15? */
#undef mat_trans_normal3
#define mat_trans_normal3(x, y, z) { \
    register float __x __asm__("fr8") = (x); \
    register float __y __asm__("fr9") = (y); \
    register float __z __asm__("fr10") = (z); \
    __asm__ __volatile__( \
                          "fldi0 fr11\n" \
                          "ftrv  xmtrx, fv8\n" \
                          : "=f" (__x), "=f" (__y), "=f" (__z) \
                          : "0" (__x), "1" (__y), "2" (__z) \
                          : "fr11"); \
    x = __x; y = __y; z = __z; \
}

GL_FORCE_INLINE PolyHeader *_glSubmissionTargetHeader(SubmissionTarget* target) {
    gl_assert(target->header_offset < aligned_vector_size(&target->output->vector));
    return aligned_vector_at(&target->output->vector, target->header_offset);
}

GL_INLINE_DEBUG Vertex* _glSubmissionTargetStart(SubmissionTarget* target) {
    gl_assert(target->start_offset < aligned_vector_size(&target->output->vector));
    return aligned_vector_at(&target->output->vector, target->start_offset);
}

Vertex* _glSubmissionTargetEnd(SubmissionTarget* target) {
    return _glSubmissionTargetStart(target) + target->count;
}

GL_FORCE_INLINE void genTriangles(Vertex* output, GLuint count) {
    Vertex* it = output + 2;

    GLuint i;
    for(i = 0; i < count; i += 3) {
        it->flags = GPU_CMD_VERTEX_EOL;
        it += 3;
    }
}

GL_FORCE_INLINE void genQuads(Vertex* output, GLuint count) {
    Vertex* pen = output + 2;
    Vertex* final = output + 3;
    GLuint i = count >> 2;
    while(i--) {
        PREFETCH(pen + 4);
        PREFETCH(final + 4);

        swapVertex(pen, final);
        final->flags = GPU_CMD_VERTEX_EOL;

        pen += 4;
        final += 4;
    }
}

GL_FORCE_INLINE void genTriangleStrip(Vertex* output, GLuint count) {
    output[count - 1].flags = GPU_CMD_VERTEX_EOL;
}

#define QUADSTRIP_COUNT(count) (((count) - 2) * 2)
static GL_NO_INLINE void genQuadStrip(Vertex* output, GLuint count) {
    Vertex* dst = output + QUADSTRIP_COUNT(count) - 1;
    Vertex* src = output + count;//(count - 1);

    for (; count > 2; count -= 2) {
        // Have to copy because of src/dst overlapping on first quad
		Vertex src1 = src[-1], src2 = src[-2], src3 = src[-3], src4 = src[-4];

        *dst   = src3;
        (*dst--).flags = GPU_CMD_VERTEX_EOL;
        *dst-- = src4;
        *dst-- = src1;
        *dst-- = src2;
        src -= 2;
    }
}

#define TRIFAN_COUNT(count) (((count) - 2) * 3)
static GL_NO_INLINE void genTriangleFan(Vertex* output, GLuint count) {
    Vertex* dst = output + TRIFAN_COUNT(count) - 1;
    Vertex* src = output + count - 1;

    // Triangles generated as {first vertex, prior vertex, current vertex}
    // e.g. {v1, v2, v3, v4} produces {v1, v2, v3}, {v1, v3, v4}
    for (; count > 2; count--) {
        *dst   = *src--;
        (*dst--).flags = GPU_CMD_VERTEX_EOL;
        *dst-- = *src;
        *dst-- = *output;
    }
}

#define POINTS_COUNT(count) ((count) * 4)
static GL_NO_INLINE void genPoints(Vertex* output, GLuint count) {
    Vertex* dst = output + POINTS_COUNT(count) - 1;
    Vertex* src = output + count - 1;
    float half_size = HALF_POINT_SIZE;

    // Expands v to { v + (S/2,-S/2), v + (S/2,S/2), v + (-S/2,-S/2), (-S/2,S/2) }
    for (; count > 0; count--, src--) {
        *dst = *src;
        dst->flags = GPU_CMD_VERTEX_EOL;
        dst->xyz[0] -= half_size; dst->xyz[1] += half_size;
        dst--;

        *dst = *src;
        dst->xyz[0] += half_size; dst->xyz[1] += half_size;
        dst--;

        *dst = *src;
        dst->xyz[0] -= half_size; dst->xyz[1] -= half_size;
        dst--;

        *dst = *src;
        dst->xyz[0] += half_size; dst->xyz[1] -= half_size;
        dst--;
    }
}

// Heavily based on the pvrline example by jnmartin84
// Which is based on https://devcry.heiho.net/html/2017/20170820-opengl-line-drawing.html
static Vertex* draw_line(Vertex* dst, Vertex* v1, Vertex* v2) {
    Vertex ov1 = *v1;
    Vertex ov2 = *v2;
    // TODO don't copy unless dst might overlap v1/v2 

	// Essentially "expands" a line into a quad by
    // 1) Calculating normal of the line from v1 to v2
	// 2) Scaling normal by the line width
	// 3) Offseting the endpoints wrt the scaled normal
    float dx = ov2.xyz[0] - ov1.xyz[0];
    float dy = ov2.xyz[1] - ov1.xyz[1];

    float inverse_mag = fast_rsqrt((dx*dx) + (dy*dy)) * HALF_LINE_WIDTH;
    float nx = -dy * inverse_mag;
    float ny =  dx * inverse_mag;

    *dst = ov2;
    dst->flags = GPU_CMD_VERTEX_EOL;
    dst->xyz[0] -= nx;
    dst->xyz[1] -= ny;
    dst--;

    *dst = ov1;
    dst->xyz[0] -= nx;
    dst->xyz[1] -= ny;
    dst--;

    *dst = ov2;
    dst->xyz[0] += nx;
    dst->xyz[1] += ny;
    dst--;

    *dst = ov1;
    dst->xyz[0] += nx;
    dst->xyz[1] += ny;
    dst--;

    return dst;
}

#define LINES_COUNT(count) (((count) / 2) * 4)
static GL_NO_INLINE void genLines(Vertex* output, GLuint count) {
    Vertex* dst = output + LINES_COUNT(count) - 1;
    Vertex* src = output + count - 1;

    // Draws line using two vertices
    for (; count >= 2; count -= 2, src -= 2) {
        dst = draw_line(dst, src, src - 1);
    }
}

#define LINE_STRIP_COUNT(count) (((count) - 1) * 4)
static GL_NO_INLINE void genLineStrip(Vertex* output, GLuint count) {
    Vertex* dst = output + LINE_STRIP_COUNT(count) - 1;
    Vertex* src = output + count - 1;

    // Draws line using current and prior vertex
    for (; count > 1; count--, src--) {
        dst = draw_line(dst, src, src - 1);
    }
}

#define LINE_LOOP_COUNT(count) ((count) * 4)
static GL_NO_INLINE void genLineLoop(Vertex* output, GLuint count) {
    Vertex* dst = output + LINE_LOOP_COUNT(count) - 1;
    Vertex* src = output + count - 1;
	Vertex last = *src, first = *output;
	
    // Draws line using current and prior vertex
    for (; count > 1; count--, src--) {
        dst = draw_line(dst, src, src - 1);
    }

    // Connect first and last vertex
	draw_line(dst, &first, &last);
}

static void _readPositionData(const GLuint first, const GLuint count, Vertex* it) {
    const ReadAttributeFunc func = ATTRIB_LIST.vertex_func;
    const GLsizei vstride = ATTRIB_LIST.vertex.stride;
    const GLubyte* vptr = ((GLubyte*) ATTRIB_LIST.vertex.ptr + (first * vstride));

    ITERATE(count) {
        PREFETCH(vptr + vstride);
        func(vptr, (GLubyte*) it);
        it->flags = GPU_CMD_VERTEX;

        vptr += vstride;
        ++it;
    }
}

static void _readUVData(const GLuint first, const GLuint count, Vertex* it) {
    const ReadAttributeFunc func = ATTRIB_LIST.uv_func;
    const GLsizei uvstride = ATTRIB_LIST.uv.stride;
    const GLubyte* uvptr = ((GLubyte*) ATTRIB_LIST.uv.ptr + (first * uvstride));

    ITERATE(count) {
        PREFETCH(uvptr + uvstride);

        func(uvptr, (GLubyte*) it->uv);
        uvptr += uvstride;
        ++it;
    }
}

static void _readSTData(const GLuint first, const GLuint count, VertexExtra* it) {
    const ReadAttributeFunc func = ATTRIB_LIST.st_func;
    const GLsizei ststride = ATTRIB_LIST.st.stride;
    const GLubyte* stptr = ((GLubyte*) ATTRIB_LIST.st.ptr + (first * ststride));

    ITERATE(count) {
        PREFETCH(stptr + ststride);
        func(stptr, (GLubyte*) it->st);
        stptr += ststride;
        ++it;
    }
}

static void _readNormalData(const GLuint first, const GLuint count, VertexExtra* it) {
    const ReadAttributeFunc func = ATTRIB_LIST.normal_func;
    const GLsizei nstride = ATTRIB_LIST.normal.stride;
    const GLubyte* nptr = ((GLubyte*) ATTRIB_LIST.normal.ptr + (first * nstride));

    ITERATE(count) {
        func(nptr, (GLubyte*) it->nxyz);
        nptr += nstride;

        if(_glIsNormalizeEnabled()) {
            GLfloat* n = (GLfloat*) it->nxyz;
            float temp = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];

            float ilength = MATH_fsrra(temp);
            n[0] *= ilength;
            n[1] *= ilength;
            n[2] *= ilength;
        }

        ++it;
    }
}

static void _readDiffuseData(const GLuint first, const GLuint count, Vertex* it) {
    const ReadAttributeFunc func = ATTRIB_LIST.colour_func;
    const GLuint cstride = ATTRIB_LIST.colour.stride;
    const GLubyte* cptr = ((GLubyte*) ATTRIB_LIST.colour.ptr) + (first * cstride);

    ITERATE(count) {
        PREFETCH(cptr + cstride);
        func(cptr, it->bgra);
        cptr += cstride;
        ++it;
    }
}

static void generateElements(
        SubmissionTarget* target, const GLsizei first, const GLuint count,
        const GLubyte* indices, const GLenum type) {

    const GLsizei istride = index_size(type);
    const IndexParseFunc IndexFunc = _calcParseIndexFunc(type);

    GLubyte* xyz;
    GLubyte* uv;
    GLubyte* bgra;
    GLubyte* st;
    GLubyte* nxyz;

    Vertex* output = _glSubmissionTargetStart(target);
    VertexExtra* ve = aligned_vector_at(target->extras, 0);

    uint32_t i = first;
    uint32_t idx = 0;

    const ReadAttributeFunc pos_func = ATTRIB_LIST.vertex_func;
    const GLsizei vstride = ATTRIB_LIST.vertex.stride;

    const ReadAttributeFunc uv_func = ATTRIB_LIST.uv_func;
    const GLuint uvstride = ATTRIB_LIST.uv.stride;

    const ReadAttributeFunc st_func = ATTRIB_LIST.st_func;
    const GLuint ststride = ATTRIB_LIST.st.stride;

    const ReadAttributeFunc diffuse_func = ATTRIB_LIST.colour_func;
    const GLuint dstride = ATTRIB_LIST.colour.stride;

    const ReadAttributeFunc normal_func = ATTRIB_LIST.normal_func;
    const GLuint nstride = ATTRIB_LIST.normal.stride;

    for(; i < first + count; ++i) {
        idx = IndexFunc(indices + (i * istride));

        xyz = (GLubyte*) ATTRIB_LIST.vertex.ptr + (idx * vstride);
        uv = (GLubyte*) ATTRIB_LIST.uv.ptr + (idx * uvstride);
        bgra = (GLubyte*) ATTRIB_LIST.colour.ptr + (idx * dstride);
        st = (GLubyte*) ATTRIB_LIST.st.ptr + (idx * ststride);
        nxyz = (GLubyte*) ATTRIB_LIST.normal.ptr + (idx * nstride);

        pos_func(xyz, (GLubyte*) output);
        uv_func(uv, (GLubyte*) output->uv);
        diffuse_func(bgra, output->bgra);
        st_func(st, (GLubyte*) ve->st);
        normal_func(nxyz, (GLubyte*) ve->nxyz);

        output->flags = GPU_CMD_VERTEX;
        ++output;
        ++ve;
    }
}

typedef struct {
    float x, y, z;
} Float3;

typedef struct {
    float u, v;
} Float2;

static const Float3 F3Z = {0.0f, 0.0f, 1.0f};
static const Float2 F2ZERO = {0.0f, 0.0f};

static void generateElementsFastPath(
        SubmissionTarget* target, const GLsizei first, const GLuint count,
        const GLubyte* indices, const GLenum type) {

    Vertex* start = _glSubmissionTargetStart(target);

    const GLuint vstride = ATTRIB_LIST.vertex.stride;
    const GLuint uvstride = ATTRIB_LIST.uv.stride;
    const GLuint ststride = ATTRIB_LIST.st.stride;
    const GLuint dstride = ATTRIB_LIST.colour.stride;
    const GLuint nstride = ATTRIB_LIST.normal.stride;

    const GLsizei istride = index_size(type);
    const IndexParseFunc IndexFunc = _calcParseIndexFunc(type);

    /* Copy the pos, uv and color directly in one go */
    const GLubyte* pos = (ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG) ? ATTRIB_LIST.vertex.ptr : NULL;
    const GLubyte* uv  = (ATTRIB_LIST.enabled & UV_ENABLED_FLAG) ? ATTRIB_LIST.uv.ptr : NULL;
    const GLubyte* col = (ATTRIB_LIST.enabled & DIFFUSE_ENABLED_FLAG) ? ATTRIB_LIST.colour.ptr : NULL;
    const GLubyte* st  = (ATTRIB_LIST.enabled & ST_ENABLED_FLAG) ? ATTRIB_LIST.st.ptr : NULL;
    const GLubyte* n   = (ATTRIB_LIST.enabled & NORMAL_ENABLED_FLAG) ? ATTRIB_LIST.normal.ptr : NULL;

    VertexExtra* ve = aligned_vector_at(target->extras, 0);
    Vertex* it = start;

    if(!pos) {
        return;
    }

    for(GLuint i = first; i < first + count; ++i) {
        GLuint idx = IndexFunc(indices + (i * istride));

        it->flags = GPU_CMD_VERTEX;

        pos = (GLubyte*) ATTRIB_LIST.vertex.ptr + (idx * vstride);
        TransformVertex(((float*) pos)[0], ((float*) pos)[1], ((float*) pos)[2], 1.0f, it->xyz, &it->w);

        if(uv) {
            uv = (GLubyte*) ATTRIB_LIST.uv.ptr + (idx * uvstride);
            MEMCPY4(it->uv, uv, sizeof(float) * 2);
        } else {
            *((Float2*) it->uv) = F2ZERO;
        }

        if(col) {
            col = (GLubyte*) ATTRIB_LIST.colour.ptr + (idx * dstride);
            MEMCPY4(it->bgra, col, sizeof(uint32_t));
        } else {
            *((uint32_t*) it->bgra) = ~0;
        }

        if(st) {
            st = (GLubyte*) ATTRIB_LIST.st.ptr + (idx * ststride);
            MEMCPY4(ve->st, st, sizeof(float) * 2);
        } else {
            *((Float2*) ve->st) = F2ZERO;
        }

        if(n) {
            n = (GLubyte*) ATTRIB_LIST.normal.ptr + (idx * nstride);
            MEMCPY4(ve->nxyz, n, sizeof(float) * 3);
        } else {
            *((Float3*) ve->nxyz) = F3Z;
        }

        it++;
        ve++;
    }
}

#define likely(x)      __builtin_expect(!!(x), 1)

#define POLYMODE ALL
#define PROCESS_VERTEX_FLAGS(it, i) { \
    (it)->flags = GPU_CMD_VERTEX; \
}

#include "draw_fastpath.inc"
#undef PROCESS_VERTEX_FLAGS
#undef POLYMODE

#define POLYMODE QUADS
#define PROCESS_VERTEX_FLAGS(it, i) { \
    it->flags = GPU_CMD_VERTEX; \
    if(((i + 1) % 4) == 0) { \
        Vertex t = *it; \
        *it = *(it - 1); \
        *(it - 1) = t; \
        it->flags = GPU_CMD_VERTEX_EOL; \
    } \
}

#include "draw_fastpath.inc"
#undef PROCESS_VERTEX_FLAGS
#undef POLYMODE

#define POLYMODE TRIS
#define PROCESS_VERTEX_FLAGS(it, i) { \
    it->flags = ((i + 1) % 3 == 0) ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX; \
}
#include "draw_fastpath.inc"
#undef PROCESS_VERTEX_FLAGS
#undef POLYMODE

static void generateArrays(SubmissionTarget* target, const GLsizei first, const GLuint count) {
    Vertex* start = _glSubmissionTargetStart(target);
    VertexExtra* ve = aligned_vector_at(target->extras, 0);

    _readPositionData(first, count, start);
    _readDiffuseData(first, count, start);
    _readUVData(first, count, start);
    _readNormalData(first, count, ve);
    _readSTData(first, count, ve);
}

/* ---- Patch C: Specialized fast-path for Position + UV + Color only ----
 * Skips ST and Normal loops entirely when those attributes are disabled.
 * This is the most common case for 2D raylib work (sprites, shapes, text, UI).
 * Saves ~40% of the per-vertex loop work compared to the generic fast path.
 */

#define ATTR_MASK_PUC (VERTEX_ENABLED_FLAG | UV_ENABLED_FLAG | DIFFUSE_ENABLED_FLAG)

/* Input prefetch distance for the SoA passes below: a fixed two cache lines
   ahead. The old (stride << 1) forms were near no-ops for the real streams
   (uv 8B / color 4B / position 12B land 8-24B ahead — INSIDE the line already
   being read); +64 always requests a future line. */
#define PUC_PREF_AHEAD 64

/* Radial fog normally rides the PUC writer below: source X/Z and the freshly
   MOVCA'd output line are already hot, so there is no second vertex-stream
   walk. The generic generator retains a post-pass fallback farther down. */
GL_FORCE_INLINE float _glRadialFogAt(const GLdcRadialFogState* fog,
                                     float x, float z) {
    const float dx = x - fog->center_x;
    const float dz = z - fog->center_z;
    return (dx * dx + dz * dz) * fog->inv_radius2;
}

GL_FORCE_INLINE unsigned _glRadialFogAmountQuartic(
        const GLdcRadialFogState* fog, float x, float z) {
    const float at = _glRadialFogAt(fog, x, z);
    if(at <= 0.0f) return 0;
    if(at >= 1.0f) return fog->amount[GLDC_RADIAL_FOG_LUT_N];
    return (unsigned)(fog->amount_scale * at * at + 0.5f);
}

GL_FORCE_INLINE unsigned _glRadialFogAmount(const GLdcRadialFogState* fog,
                                             float x, float z) {
    if(fog->quartic_curve)
        return _glRadialFogAmountQuartic(fog, x, z);

    const float at = _glRadialFogAt(fog, x, z);
    if(at <= 0.0f) return 0;
    if(at >= 1.0f) return fog->amount[GLDC_RADIAL_FOG_LUT_N];
    const float fi = at * (float)GLDC_RADIAL_FOG_LUT_N;
    const unsigned li = (unsigned)fi;
    const float a0 = (float)fog->amount[li];
    return (unsigned)(a0 +
        ((float)fog->amount[li + 1] - a0) * (fi - (float)li) + 0.5f);
}

GL_FORCE_INLINE void _glWriteRadialFogAmount(Vertex* out,
                                             const GLdcRadialFogState* fog,
                                             unsigned amount) {
    if(fog->mode == GL_KOS_VERTEX_FOG_BLEND) {
        out->bgra[3] = (GLubyte)amount;
    } else if(fog->mode == GL_KOS_VERTEX_FOG_ATTENUATE_ALPHA) {
        const unsigned keep = 255u - amount;
        out->bgra[3] = (GLubyte)
            (((unsigned)out->bgra[3] * keep + 127u) / 255u);
    } else {
        const unsigned keep = 255u - amount;
        out->bgra[0] = (GLubyte)
            (((unsigned)out->bgra[0] * keep +
              (unsigned)fog->color_b * amount + 127u) / 255u);
        out->bgra[1] = (GLubyte)
            (((unsigned)out->bgra[1] * keep +
              (unsigned)fog->color_g * amount + 127u) / 255u);
        out->bgra[2] = (GLubyte)
            (((unsigned)out->bgra[2] * keep +
              (unsigned)fog->color_r * amount + 127u) / 255u);
    }
}

GL_FORCE_INLINE void _glWriteRadialFog(Vertex* out,
                                       const GLdcRadialFogState* fog,
                                       float x, float z) {
    _glWriteRadialFogAmount(out, fog, _glRadialFogAmount(fog, x, z));
}

/* City walls, signs, poles and many roof details are vertical quads: source
   2 repeats source 1's X/Z and source 3 repeats source 0's X/Z. Reuse those
   exact coefficients; general roof quads fall through to four evaluations. */
GL_FORCE_INLINE void _glWriteRadialFogQuad(Vertex* out,
                                           const GLdcRadialFogState* fog,
                                           const GLubyte* pp,
                                           GLuint stride) {
    const float* p0 = (const float*)pp;
    const float* p1 = (const float*)(pp + stride);
    const float* p2 = (const float*)(pp + (stride << 1));
    const float* p3 = (const float*)(pp + stride * 3);
    unsigned a0, a1, a2, a3;
    if(fog->quartic_curve) {
        a0 = _glRadialFogAmountQuartic(fog, p0[0], p0[2]);
        a1 = _glRadialFogAmountQuartic(fog, p1[0], p1[2]);
        a2 = (p2[0] == p1[0] && p2[2] == p1[2])
            ? a1 : _glRadialFogAmountQuartic(fog, p2[0], p2[2]);
        a3 = (p3[0] == p0[0] && p3[2] == p0[2])
            ? a0 : _glRadialFogAmountQuartic(fog, p3[0], p3[2]);
    } else {
        a0 = _glRadialFogAmount(fog, p0[0], p0[2]);
        a1 = _glRadialFogAmount(fog, p1[0], p1[2]);
        a2 = (p2[0] == p1[0] && p2[2] == p1[2])
            ? a1 : _glRadialFogAmount(fog, p2[0], p2[2]);
        a3 = (p3[0] == p0[0] && p3[2] == p0[2])
            ? a0 : _glRadialFogAmount(fog, p3[0], p3[2]);
    }
    if(fog->mode == GL_KOS_VERTEX_FOG_BLEND) {
        out[0].bgra[3] = (GLubyte)a0;
        out[1].bgra[3] = (GLubyte)a1;
        out[3].bgra[3] = (GLubyte)a2;
        out[2].bgra[3] = (GLubyte)a3;
        return;
    }
    _glWriteRadialFogAmount(out + 0, fog, a0);
    _glWriteRadialFogAmount(out + 1, fog, a1);
    _glWriteRadialFogAmount(out + 3, fog, a2);
    _glWriteRadialFogAmount(out + 2, fog, a3);
}

typedef struct {
    const GLubyte* positions;
    const GLubyte* uvs;
    const GLubyte* colors;
    GLuint position_stride;
    GLuint uv_stride;
    GLuint color_stride;
    GLboolean positions_are_float;
} PUCQuadInput;

/* One quad writer serves both ordinary client arrays and the typed borrowed
   raylib lane. Keeping the scheduled small/pair/GOLD kernels here prevents the
   adapter path from growing a subtly different swizzle, fog or EOL contract. */
static void _glWritePUCQuads(SubmissionTarget* target,
                             const PUCQuadInput* input,
                             const GLuint count) {
    Vertex* const batch_base = (Vertex*) _glSubmissionTargetStart(target);
    const GLdcRadialFogState* const radial = _glRadialVertexFog();
    const GLboolean radial_active =
        radial->mode != GL_KOS_VERTEX_FOG_OFF &&
        radial->mode != GL_KOS_VERTEX_FOG_BLEND_PRECOMPUTED &&
        input->positions_are_float;

    const GLuint pstride = input->position_stride;
    const GLuint ustride = input->uv_stride;
    const GLuint cstride = input->color_stride;

    /* ---- Fused four-vertex kernel (2026-07-16, the city lane) ----
       The three-pass SoA body below touches every output line three times
       (uv, color, position) with the swizzle recomputed per pass; between
       passes a MOVCA'd line can be evicted and read back. This kernel writes
       each quad's four records COMPLETELY, one MOVCA + full 32-byte fill per
       record, strip order (0,1,3,2) baked into the slot offsets, EOL on the
       last strip record. Requires the full P3F/T2F/C4UB-aligned set — the
       city and glow scratch always are; anything else takes the passes. */
    {
        const GLubyte* pp = input->positions;
        const GLubyte* up = input->uvs;
        const GLubyte* cp = input->colors;

        /* Untextured colored quads: fuse zero-UV fill, color copy, transform
           and strip swizzle into one pass.
           The old fallback touched every output cache line three times even
           though the UV stream was disabled. */
        if(!up && cp && ((((uintptr_t) cp) | cstride) & 3) == 0) {
            Vertex* it = batch_base;

#define PC_Q_PAIR(sa, sb, fa, fb) do { \
        Vertex* da = it + (sa); \
        Vertex* db = it + (sb); \
        VERTEX_CACHE_ALLOC(da); \
        VERTEX_CACHE_ALLOC(db); \
        const float* qa = (const float*) pp; \
        const float* qb = (const float*) (pp + pstride); \
        TransformVertex2(qa[0], qa[1], qa[2], da->xyz, &da->w, \
                         qb[0], qb[1], qb[2], db->xyz, &db->w); \
        da->uv[0] = 0.0f; da->uv[1] = 0.0f; \
        db->uv[0] = 0.0f; db->uv[1] = 0.0f; \
        *((uint32_t*) da->bgra) = *((const uint32_t*) cp); \
        *((uint32_t*) db->bgra) = *((const uint32_t*) (cp + cstride)); \
        da->flags = (fa); \
        db->flags = (fb); \
        pp += pstride << 1; cp += cstride << 1; \
    } while(0)

            for(GLuint q = count >> 2; q--; it += 4) {
                PREFETCH(pp + PUC_PREF_AHEAD);
                PREFETCH(cp + PUC_PREF_AHEAD);
                const GLubyte* const qpp = pp;
                PC_Q_PAIR(0, 1, GPU_CMD_VERTEX, GPU_CMD_VERTEX);
                PC_Q_PAIR(3, 2, GPU_CMD_VERTEX_EOL, GPU_CMD_VERTEX);
                if(radial_active)
                    _glWriteRadialFogQuad(it, radial, qpp, pstride);
            }
#undef PC_Q_PAIR

            /* Defensive tail for non-quad-aligned callers. GL_QUADS normally
               rejects such input before this point; initialize any reserved
               records anyway, matching the established fallback flags. */
            for(GLuint r = 0; r < (count & 3); ++r, ++it) {
                VERTEX_CACHE_ALLOC(it);
                TransformVertex(((const float*) pp)[0], ((const float*) pp)[1],
                                ((const float*) pp)[2], 1.0f, it->xyz, &it->w);
                it->uv[0] = 0.0f; it->uv[1] = 0.0f;
                *((uint32_t*) it->bgra) = *((const uint32_t*) cp);
                if(radial_active)
                    _glWriteRadialFog(it, radial, ((const float*)pp)[0],
                                      ((const float*)pp)[2]);
                it->flags = (r == 2) ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX;
                pp += pstride; cp += cstride;
            }
            return;
        }

        if(up && cp && ((((uintptr_t) cp) | cstride) & 3) == 0) {
            Vertex* it = batch_base;

#define PUC_Q_VERT(slot, fl) do { \
        Vertex* d = it + (slot); \
        VERTEX_CACHE_ALLOC(d); \
        TransformVertex(((const float*) pp)[0], ((const float*) pp)[1], \
                        ((const float*) pp)[2], 1.0f, d->xyz, &d->w); \
        d->uv[0] = ((const float*) up)[0]; \
        d->uv[1] = ((const float*) up)[1]; \
        *((uint32_t*) d->bgra) = *((const uint32_t*) cp); \
        d->flags = (fl); \
        pp += pstride; up += ustride; cp += cstride; \
    } while(0)

            /* Glyph/bar-scale draws (HUD): the pair machinery's setup overhead
               outweighs the FTRV win — [PROF] measured hud= +0.25ms when small
               batches rode the pairs. They take the proven single-vertex path. */
            if(count < 64) {
                for(GLuint q = count >> 2; q--; it += 4) {
                    const GLubyte* const qpp = pp;
                    PUC_Q_VERT(0, GPU_CMD_VERTEX);
                    PUC_Q_VERT(1, GPU_CMD_VERTEX);
                    PUC_Q_VERT(3, GPU_CMD_VERTEX_EOL);
                    PUC_Q_VERT(2, GPU_CMD_VERTEX);
                    if(radial_active)
                        _glWriteRadialFogQuad(it, radial, qpp, pstride);
                }
                for(GLuint r = 0; r < (count & 3); ++r, ++it) {
                    const float* const rp = (const float*)pp;
                    PUC_Q_VERT(0, (r == 2) ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX);
                    if(radial_active)
                        _glWriteRadialFog(it, radial, rp[0], rp[2]);
                }
                return;   /* (PUC_Q_VERT's #undef stays at the block end below) */
            }

#ifdef HAVE_GOLD_PAIR
/* The scheduled block does loads, both FTRVs, MOVCA and ALL stores itself —
   uv loads and record stores ride the FTRV latency (see TransformFillPair). */
#define PUC_Q_PAIR(sa, sb, fa, fb) do { \
        TransformFillPair((const float*) pp, (const float*) (pp + pstride), \
                          (const float*) up, (const float*) (up + ustride), \
                          *((const uint32_t*) cp), *((const uint32_t*) (cp + cstride)), \
                          (fa), (fb), it + (sa), it + (sb)); \
        pp += pstride << 1; up += ustride << 1; cp += cstride << 1; \
    } while(0)
#else
/* Two source vertices per shot through the dual-FTRV pair (fv4+fv8): the
   second FTRV issues while the first drains, and the eight input loads /
   eight result stores schedule around the block instead of serializing. */
#define PUC_Q_PAIR(sa, sb, fa, fb) do { \
        Vertex* da = it + (sa); \
        Vertex* db = it + (sb); \
        VERTEX_CACHE_ALLOC(da); \
        VERTEX_CACHE_ALLOC(db); \
        const float* qa = (const float*) pp; \
        const float* qb = (const float*) (pp + pstride); \
        TransformVertex2(qa[0], qa[1], qa[2], da->xyz, &da->w, \
                         qb[0], qb[1], qb[2], db->xyz, &db->w); \
        da->uv[0] = ((const float*) up)[0]; \
        da->uv[1] = ((const float*) up)[1]; \
        db->uv[0] = ((const float*) (up + ustride))[0]; \
        db->uv[1] = ((const float*) (up + ustride))[1]; \
        *((uint32_t*) da->bgra) = *((const uint32_t*) cp); \
        *((uint32_t*) db->bgra) = *((const uint32_t*) (cp + cstride)); \
        da->flags = (fa); \
        db->flags = (fb); \
        pp += pstride << 1; up += ustride << 1; cp += cstride << 1; \
    } while(0)
#endif

#if GLDC_GOLD_BLOCK
            /* B2 GOLD-BLOCK (2026-07-24): one whole quad per scheduled block —
               four FTRVs in true flight across all four FP banks, swizzle+EOL
               baked as record offsets. The stride adjusts are loop-invariant. */
            {
                const int padj = (int)pstride - 12;
                const int uadj = (int)ustride - 8;
                const int cadj = (int)cstride - 4;
                if(radial_active) {
                    for(GLuint q = count >> 2; q--; it += 4) {
                        PREFETCH(pp + PUC_PREF_AHEAD);
                        PREFETCH(up + PUC_PREF_AHEAD);
                        PREFETCH(cp + PUC_PREF_AHEAD);
                        TransformFillQuad(pp, up, cp, padj, uadj, cadj,
                                          GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL, it);
                        _glWriteRadialFogQuad(it, radial, pp, pstride);
                        pp += pstride << 2;
                        up += ustride << 2;
                        cp += cstride << 2;
                    }
                } else {
                    for(GLuint q = count >> 2; q--; it += 4) {
                        PREFETCH(pp + PUC_PREF_AHEAD);
                        PREFETCH(up + PUC_PREF_AHEAD);
                        PREFETCH(cp + PUC_PREF_AHEAD);
                        TransformFillQuad(pp, up, cp, padj, uadj, cadj,
                                          GPU_CMD_VERTEX, GPU_CMD_VERTEX_EOL, it);
                        pp += pstride << 2;
                        up += ustride << 2;
                        cp += cstride << 2;
                    }
                }
            }
#else
            for(GLuint q = count >> 2; q--; it += 4) {
                PREFETCH(pp + PUC_PREF_AHEAD);
                PREFETCH(up + PUC_PREF_AHEAD);
                PREFETCH(cp + PUC_PREF_AHEAD);
                const GLubyte* const qpp = pp;
                PUC_Q_PAIR(0, 1, GPU_CMD_VERTEX, GPU_CMD_VERTEX);
                PUC_Q_PAIR(3, 2, GPU_CMD_VERTEX_EOL, GPU_CMD_VERTEX);   /* src 2 = last strip record */
                if(radial_active)
                    _glWriteRadialFogQuad(it, radial, qpp, pstride);
            }
#endif
#undef PUC_Q_PAIR

            /* Partial trailing quad (caller contract violation — the city never
               sends one): the records are already reserved on the list, so they
               must be initialized. Sequential, old flag pattern; the submit
               finalizer's generic path EOL-handles the unterminated span. */
            for(GLuint r = 0; r < (count & 3); ++r, ++it) {
                const float* const rp = (const float*)pp;
                PUC_Q_VERT(0, (r == 2) ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX);
                if(radial_active)
                    _glWriteRadialFog(it, radial, rp[0], rp[2]);
            }
#undef PUC_Q_VERT
            return;
        }
    }

    /* Fallback: the original three-pass SoA body (missing uv/color, or
       unaligned colors). */
    GLuint min = 0;
    for(min = 0; min < count; min += 60) {
        Vertex* const start = batch_base + min;
        const int_fast32_t loop = ((min + 60) > count) ? count - min : 60;
        const int offset = (int)min;
        Vertex* it;
        GLuint stride;
        const GLubyte* ptr;

        /* UV — the FIRST writer of each output vertex: allocate its cache line
           (exactly one line per 32B-aligned Vertex, MOVCA.L) so this write stream
           never reads RAM it is about to overwrite; every field gets filled across
           the UV/color/position passes below. Prefetch the input stream ahead. */
        stride = input->uv_stride;
        ptr = input->uvs ? input->uvs + (offset * stride) : NULL;
        it = start;
        /* Destinations are SWIZZLED to PVR strip order (0,1,3,2 within each quad,
           see PUC_DST below): writing the final order directly removes the old
           two-record 32-byte swap per quad (>=128B of cache traffic each). Batch
           starts are quad-aligned (60 %% 4 == 0), so the swizzle never crosses a
           batch boundary. */
#define PUC_DST(base, i) ((base) + ((i) ^ ((((i) & 3) == 2 || (((i) & 3) == 3)) ? 1 : 0)))
        if(ptr) {
            for(int_fast32_t i = 0; i < loop; ++i) {
                Vertex* dst = PUC_DST(it, i);
                PREFETCH(ptr + PUC_PREF_AHEAD);
                VERTEX_CACHE_ALLOC(dst);
                dst->uv[0] = ((float*) ptr)[0];
                dst->uv[1] = ((float*) ptr)[1];
                ptr += stride;
            }
        } else {
            for(int_fast32_t i = 0; i < loop; ++i) {
                Vertex* dst = PUC_DST(it, i);
                VERTEX_CACHE_ALLOC(dst);
                dst->uv[0] = 0; dst->uv[1] = 0;
            }
        }

        /* Color */
        stride = input->color_stride;
        ptr = input->colors ? input->colors + (offset * stride) : NULL;
        it = start;
        if(ptr) {
            if(((((uintptr_t) ptr) | stride) & 3) == 0) {
                /* aligned client colors (the common case): one word copy, not 4 byte ops */
                for(int_fast32_t i = 0; i < loop; ++i) {
                    PREFETCH(ptr + PUC_PREF_AHEAD);
                    *((uint32_t*) PUC_DST(it, i)->bgra) = *((const uint32_t*) ptr);
                    ptr += stride;
                }
            } else {
                for(int_fast32_t i = 0; i < loop; ++i) {
                    Vertex* dst = PUC_DST(it, i);
                    dst->bgra[0] = ptr[0]; dst->bgra[1] = ptr[1];
                    dst->bgra[2] = ptr[2]; dst->bgra[3] = ptr[3];
                    ptr += stride;
                }
            }
        } else {
            for(int_fast32_t i = 0; i < loop; ++i) {
                *((uint32_t*) PUC_DST(it, i)->bgra) = ~0;
            }
        }

        /* Position + transform + quad vertex flags */
        stride = input->position_stride;
        ptr = input->positions + (offset * stride);
        it = start;
        for(int_fast32_t i = 0; i < loop; ++i) {
            Vertex* dst = PUC_DST(it, i);
            PREFETCH(ptr + PUC_PREF_AHEAD);
            TransformVertex(((float*) ptr)[0], ((float*) ptr)[1], ((float*) ptr)[2], 1.0f, dst->xyz, &dst->w);
            if(radial_active)
                _glWriteRadialFog(dst, radial, ((float*)ptr)[0],
                                  ((float*)ptr)[2]);
            /* strip-order slot 3 (source vertex 2) carries EOL — no record swap needed */
            dst->flags = (((i & 3) == 2) ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX);
            ptr += stride;
        }
#undef PUC_DST

        /* ST and Normal loops: SKIPPED — not enabled */
    }
}

static void generateArraysFastPath_PUC_QUADS(SubmissionTarget* target,
                                              const GLsizei first,
                                              const GLuint count) {
    if(!(ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG)) return;

    const PUCQuadInput input = {
        .positions = ATTRIB_LIST.vertex.ptr + first * ATTRIB_LIST.vertex.stride,
        .uvs = (ATTRIB_LIST.enabled & UV_ENABLED_FLAG)
            ? ATTRIB_LIST.uv.ptr + first * ATTRIB_LIST.uv.stride : NULL,
        .colors = (ATTRIB_LIST.enabled & DIFFUSE_ENABLED_FLAG)
            ? ATTRIB_LIST.colour.ptr + first * ATTRIB_LIST.colour.stride : NULL,
        .position_stride = ATTRIB_LIST.vertex.stride,
        .uv_stride = ATTRIB_LIST.uv.stride,
        .color_stride = ATTRIB_LIST.colour.stride,
        .positions_are_float = ATTRIB_LIST.vertex.type == GL_FLOAT
    };
    _glWritePUCQuads(target, &input, count);
}

static void generateArraysFastPath_PUC_TRIS(SubmissionTarget* target, const GLsizei first, const GLuint count) {
    if(!(ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG)) return;

    Vertex* const batch_base = (Vertex*) _glSubmissionTargetStart(target);
    const GLdcRadialFogState* const radial = _glRadialVertexFog();
    const GLboolean radial_active =
        radial->mode != GL_KOS_VERTEX_FOG_OFF &&
        radial->mode != GL_KOS_VERTEX_FOG_BLEND_PRECOMPUTED &&
        ATTRIB_LIST.vertex.type == GL_FLOAT;

    GLuint min = 0;
    for(min = 0; min < count; min += 60) {
        Vertex* const start = batch_base + min;
        const int_fast32_t loop = ((min + 60) > count) ? count - min : 60;
        const int offset = (first + min);
        Vertex* it;
        GLuint stride;
        const GLubyte* ptr;

        /* UV */
        stride = ATTRIB_LIST.uv.stride;
        ptr = (ATTRIB_LIST.enabled & UV_ENABLED_FLAG) ? ATTRIB_LIST.uv.ptr + (offset * stride) : NULL;
        it = start;
        /* First writer of each output vertex: MOVCA the line, PREF the input
           (same treatment as PUC_QUADS, 2026-07-15). */
        if(ptr) {
            for(int_fast32_t i = 0; i < loop; ++i, ++it) {
                PREFETCH(ptr + PUC_PREF_AHEAD);
                VERTEX_CACHE_ALLOC(it);
                it->uv[0] = ((float*) ptr)[0];
                it->uv[1] = ((float*) ptr)[1];
                ptr += stride;
            }
        } else {
            for(int_fast32_t i = 0; i < loop; ++i, ++it) {
                VERTEX_CACHE_ALLOC(it);
                it->uv[0] = 0; it->uv[1] = 0;
            }
        }

        /* Color */
        stride = ATTRIB_LIST.colour.stride;
        ptr = (ATTRIB_LIST.enabled & DIFFUSE_ENABLED_FLAG) ? ATTRIB_LIST.colour.ptr + (offset * stride) : NULL;
        it = start;
        if(ptr) {
            if(((((uintptr_t) ptr) | stride) & 3) == 0) {
                for(int_fast32_t i = 0; i < loop; ++i, ++it) {
                    PREFETCH(ptr + PUC_PREF_AHEAD);
                    *((uint32_t*) it->bgra) = *((const uint32_t*) ptr);
                    ptr += stride;
                }
            } else {
                for(int_fast32_t i = 0; i < loop; ++i, ++it) {
                    it->bgra[0] = ptr[0]; it->bgra[1] = ptr[1];
                    it->bgra[2] = ptr[2]; it->bgra[3] = ptr[3];
                    ptr += stride;
                }
            }
        } else {
            for(int_fast32_t i = 0; i < loop; ++i, ++it) {
                *((uint32_t*) it->bgra) = ~0;
            }
        }

        /* Position + transform + triangle vertex flags */
        stride = ATTRIB_LIST.vertex.stride;
        ptr = ATTRIB_LIST.vertex.ptr + (offset * stride);
        it = start;
        if(radial_active) {
            for(int_fast32_t i = 0; i < loop; ++i, ++it) {
                PREFETCH(ptr + PUC_PREF_AHEAD);
                TransformVertex(((float*) ptr)[0], ((float*) ptr)[1],
                                ((float*) ptr)[2], 1.0f, it->xyz, &it->w);
                _glWriteRadialFog(it, radial, ((float*)ptr)[0],
                                  ((float*)ptr)[2]);
                it->flags = ((min + i + 1) % 3 == 0) ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX;
                ptr += stride;
            }
        } else {
            for(int_fast32_t i = 0; i < loop; ++i, ++it) {
                PREFETCH(ptr + PUC_PREF_AHEAD);
                TransformVertex(((float*) ptr)[0], ((float*) ptr)[1], ((float*) ptr)[2], 1.0f, it->xyz, &it->w);
                it->flags = ((min + i + 1) % 3 == 0) ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX;
                ptr += stride;
            }
        }

        /* ST and Normal loops: SKIPPED — not enabled */
    }
}

/* ---- End Patch C ---- */

static void generate(SubmissionTarget* target, const GLenum mode, const GLsizei first, const GLuint count,
        const GLubyte* indices, const GLenum type) {
    /* Read from the client buffers and generate an array of ClipVertices */
    TRACE();

    if(ATTRIB_LIST.fast_path) {
        GLDC_STAT_INC(fast_path_hits);

        /* Patch C: Specialized P+UV+Color dispatch — skips ST/Normal entirely.
           submitVertices' extras-resize skip mirrors this condition — keep in
           sync. */
        if(!indices && (ATTRIB_LIST.enabled & (ST_ENABLED_FLAG | NORMAL_ENABLED_FLAG)) == 0) {
            switch(mode) {
                case GL_QUADS:
                    generateArraysFastPath_PUC_QUADS(target, first, count);
                    return;
                case GL_TRIANGLES:
                    generateArraysFastPath_PUC_TRIS(target, first, count);
                    return;
                default:
                    break;  /* Fall through to generic fast path */
            }
        }

        if(indices) {
            generateElementsFastPath(target, first, count, indices, type);
        } else {
            switch(mode) {
                case GL_QUADS:
                    generateArraysFastPath_QUADS(target, first, count);
                    return;  // Don't need to do any more processing
                case GL_TRIANGLES:
                    generateArraysFastPath_TRIS(target, first, count);
                    return; // Don't need to do any more processing
                default:
                    generateArraysFastPath_ALL(target, first, count);
            }
        }
    } else {
        GLDC_STAT_INC(fast_path_misses);
        if(indices) {
            generateElements(target, first, count, indices, type);
        } else {
            generateArrays(target, first, count);
        }
    }

    Vertex* it = _glSubmissionTargetStart(target);
    // Drawing arrays
    switch(mode) {
    case GL_TRIANGLES:
        genTriangles(it, count);
        break;
    case GL_QUADS:
        genQuads(it, count);
        break;
    case GL_TRIANGLE_STRIP:
        genTriangleStrip(it, count);
        break;

    case GL_QUAD_STRIP:
        genQuadStrip(it, count);
        break;
    case GL_TRIANGLE_FAN:
        genTriangleFan(it, count);
        break;

    case GL_POINTS:
        genPoints(it, count);
        break;
    case GL_LINES:
        genLines(it, count);
        break;
    case GL_LINE_STRIP:
        genLineStrip(it, count);
        break;
    case GL_LINE_LOOP:
        genLineLoop(it, count);
        break;
    default:
        gl_assert(0 && "Not Implemented");
    }
}

/* Apply the radial coefficient while object-space X/Z are still available.
   The scene finalizer later moves BLEND coefficients into oargb.a after W is
   consumed; ATTENUATE_ALPHA and sparse untextured RGB mixing finish here. */
static void _glApplyRadialVertexFog(SubmissionTarget* target, GLenum mode,
                                    GLsizei first, GLuint count,
                                    const GLvoid* indices) {
    const GLdcRadialFogState* fog = _glRadialVertexFog();
    if(fog->mode == GL_KOS_VERTEX_FOG_OFF ||
       fog->mode == GL_KOS_VERTEX_FOG_BLEND_PRECOMPUTED || indices ||
       (mode != GL_QUADS && mode != GL_TRIANGLES) ||
       !(ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG) ||
       ATTRIB_LIST.vertex.type != GL_FLOAT) return;

    /* The common P+UV+Color client-array generators wrote fog while X/Z and
       the destination cache line were already live. Do not walk them again. */
    if(ATTRIB_LIST.fast_path &&
       (ATTRIB_LIST.enabled & (ST_ENABLED_FLAG | NORMAL_ENABLED_FLAG)) == 0)
        return;

    const GLuint stride = ATTRIB_LIST.vertex.stride;
    const GLubyte* src = ATTRIB_LIST.vertex.ptr + (size_t)first * stride;
    Vertex* out = _glSubmissionTargetStart(target);
    for(GLuint i = 0; i < count; ++i, src += stride) {
        const float* p = (const float*)src;
        GLuint oi = i;
        if(mode == GL_QUADS)
            oi ^= (((i & 3u) == 2u || (i & 3u) == 3u) ? 1u : 0u);
        _glWriteRadialFog(out + oi, fog, p[0], p[2]);
    }
}

GL_FORCE_INLINE int _calc_pvr_face_culling() {
    if(!_glIsCullingEnabled()) {
        return GPU_CULLING_SMALL;
    } else {
        if(_glGetCullFace() == GL_BACK) {
            return (_glGetFrontFace() == GL_CW) ? GPU_CULLING_CCW : GPU_CULLING_CW;
        } else {
            return (_glGetFrontFace() == GL_CCW) ? GPU_CULLING_CCW : GPU_CULLING_CW;
        }
    }
}

GL_FORCE_INLINE int _calc_pvr_depth_test() {
    if(!_glIsDepthTestEnabled()) {
        return GPU_DEPTHCMP_ALWAYS;
    }

    switch(_glGetDepthFunc()) {
        case GL_NEVER:
            return GPU_DEPTHCMP_NEVER;
        case GL_LESS:
            return GPU_DEPTHCMP_GREATER;
        case GL_EQUAL:
            return GPU_DEPTHCMP_EQUAL;
        case GL_LEQUAL:
            return GPU_DEPTHCMP_GEQUAL;
        case GL_GREATER:
            return GPU_DEPTHCMP_LESS;
        case GL_NOTEQUAL:
            return GPU_DEPTHCMP_NOTEQUAL;
        case GL_GEQUAL:
            return GPU_DEPTHCMP_LEQUAL;
        break;
        case GL_ALWAYS:
        default:
            return GPU_DEPTHCMP_ALWAYS;
    }
}

/* Build the PolyContext for the CURRENT GL state on the given list — the state
   half of apply_poly_header, shared with the sprite path (sh4 platform), which
   compiles the same context into a sprite header instead. Populates *out_ctx
   DIRECTLY: the first extraction built a local and copied it out, adding a
   196-byte bulk copy to EVERY header emitted (2026-07-16 audit, confirmed in
   the ELF as an out-of-line __movmem call). */
void _glBuildPolyContext(PolyContext* out_ctx, PolyList* activePolyList, GLshort textureUnit) {
    GLDC_SWAP_WORK_ADD(context_builds, 1u);
#define ctx (*out_ctx)
    memset(&ctx, 0, sizeof(PolyContext));

    ctx.list_type = activePolyList->list_type;
    ctx.fmt.color = GPU_CLRFMT_ARGBPACKED;
    ctx.fmt.uv = GPU_UVFMT_32BIT;
    ctx.gen.color_clamp = GPU_CLRCLAMP_DISABLE;

    ctx.gen.culling = _calc_pvr_face_culling();
    ctx.depth.comparison = _calc_pvr_depth_test();
    ctx.depth.write = _glIsDepthWriteEnabled() ? GPU_DEPTHWRITE_ENABLE : GPU_DEPTHWRITE_DISABLE;

    ctx.gen.shading = (_glGetShadeModel() == GL_SMOOTH) ? GPU_SHADE_GOURAUD : GPU_SHADE_FLAT;
    const GLdcRadialFogState* radial_fog = _glRadialVertexFog();
    const GLboolean radial_blend =
        radial_fog->mode == GL_KOS_VERTEX_FOG_BLEND ||
        radial_fog->mode == GL_KOS_VERTEX_FOG_BLEND_PRECOMPUTED;
    ctx.gen.specular = radial_blend ? 1 : 0;

    if(_glIsScissorTestEnabled()) {
        ctx.gen.clip_mode = GPU_USERCLIP_INSIDE;
    } else {
        ctx.gen.clip_mode = GPU_USERCLIP_DISABLE;
    }

    if(radial_blend) {
        ctx.gen.fog_type = GPU_FOG_VERTEX;
    } else if(_glIsFogEnabled()) {
        ctx.gen.fog_type = GPU_FOG_TABLE;
    } else {
        ctx.gen.fog_type = GPU_FOG_DISABLE;
    }

    if(_glIsBlendingEnabled() || _glIsAlphaTestEnabled()) {
        ctx.gen.alpha = GPU_ALPHA_ENABLE;
    } else {
        ctx.gen.alpha = GPU_ALPHA_DISABLE;
    }

    if(ctx.list_type == GPU_LIST_OP_POLY) {
        /* Opaque polys are always one/zero */
        ctx.blend.src = GPU_BLEND_ONE;
        ctx.blend.dst = GPU_BLEND_ZERO;
    } else if(ctx.list_type == GPU_LIST_PT_POLY) {
        /* HOLLY2 punch-through REQUIRES SRC_Alpha/Inv_SRC_Alpha here: the TSP
           Instruction Word must specify SRC Alpha Instruction = 4 for PT polys
           (DC Dev.Box System Architecture, sec 3.7.9.2). PT pixels are still drawn
           OPAQUE ("translucent processing is not performed", sec 3.4.3) — this is a
           mandatory format, NOT a real blend, so it costs no extra fill. ONE/ZERO
           (the opaque-poly setting) violates the spec and renders transparent texels
           as opaque boxes on hardware. */
        ctx.blend.src = GPU_BLEND_SRCALPHA;
        ctx.blend.dst = GPU_BLEND_INVSRCALPHA;
        ctx.depth.comparison = GPU_DEPTHCMP_LEQUAL;
    } else {
        ctx.blend.src = _glGetGpuBlendSrcFactor();
        ctx.blend.dst = _glGetGpuBlendDstFactor();

        if(ctx.list_type == GPU_LIST_TR_POLY && AUTOSORT_ENABLED) {
            /* Autosort mode requires this mode for transparent polys */
            ctx.depth.comparison = GPU_DEPTHCMP_GEQUAL;
        }
    }

    _glUpdatePVRTextureContext(&ctx, textureUnit);
#undef ctx
}

GL_FORCE_INLINE void apply_poly_header(PolyHeader* header, GLboolean multiTextureHeader, PolyList* activePolyList, GLshort textureUnit) {
    TRACE();
    GLDC_STAT_INC(headers_emitted);

    PolyContext ctx;
    _glBuildPolyContext(&ctx, activePolyList, textureUnit);

    if(multiTextureHeader) {
        gl_assert(ctx.list_type == GPU_LIST_TR_POLY);

        ctx.gen.alpha = GPU_ALPHA_ENABLE;
        ctx.txr.alpha = GPU_TXRALPHA_ENABLE;
        ctx.blend.src = GPU_BLEND_ZERO;
        ctx.blend.dst = GPU_BLEND_DESTCOLOR;
        ctx.depth.comparison = GPU_DEPTHCMP_EQUAL;
    }

    CompilePolyHeader(header, &ctx);

    /* Force bits 18 and 19 on to switch to 6 triangle strips */
    header->cmd |= 0xC0000;

    /* Post-process the vertex list */
    /*
     * This is currently unnecessary. aligned_vector memsets the allocated objects
     * to zero, and we don't touch oargb, also, we don't *enable* oargb yet in the
     * pvr header so it should be ignored anyway. If this ever becomes a problem,
     * uncomment this.
    ClipVertex* vout = output;
    const ClipVertex* end = output + count;
    while(vout < end) {
        vout->oargb = 0;
    }
    */
}

#define DEBUG_CLIPPING 0


static AlignedVector VERTEX_EXTRAS;
static SubmissionTarget SUBMISSION_TARGET;

/* Polygon offset (PVR W-buffer) bake, shared by every submission entry: the
   perspective divide is deferred to flush (SceneListSubmit), by which point the
   global offset state is already reset — so bake the bias HERE, per draw, while
   it's valid. We want the flushed depth (1/w) pulled toward the camera by
   _glPolygonOffsetMul while screen x/y (x/w, y/w) stay put, so pre-scale clip
   x/y/w by its reciprocal:  1/(w/pom) = pom/w (depth biased);
   (x/pom)/(w/pom) = x/w (screen unchanged). Perspective verts only — the w==1
   ortho path derives depth from z, not 1/w. */
static void _glBakePolygonOffset(Vertex* v, GLuint count) {
    if(_glPolygonOffsetMul == 1.0f) return;
    GLDC_STAT_ADD(polygon_offset_vertices, count);
    const float inv = 1.0f / _glPolygonOffsetMul;
    Vertex* const end = v + count;
    for(; v < end; ++v) {
        if(v->w != 1.0f) {
            v->xyz[0] *= inv;
            v->xyz[1] *= inv;
            v->w      *= inv;
        }
    }
}

/* ---- Capture & replay (2026-07-15, HyperSolar P4: transform-once dual-list emit) ----
   The DC city deliberately submits the SAME window-stream geometry twice: OPAQUE with the
   base tiles (bit-identical depth is what kills wall z-fighting), then PUNCH-THROUGH with
   the window tiles. That re-ran the whole TnL for ~2k verts every frame. Capture remembers
   the span the next draw wrote into its poly list (post-TnL clip-space, pre-divide); replay
   clones that span into the CURRENT list under the CURRENT GPU state (header: texture,
   blend, fog), optionally overrides the per-vertex color with a constant, and re-bakes the
   CURRENT polygon-offset multiplier (the capture ran without one). Upstream sketched this
   exact idea for multitexture in the commented block at the end of submitVertices.
   Captures hold INDICES (the vectors realloc as they grow) and are invalidated every swap
   (the lists are cleared then — a stale replay would read recycled memory). */
/* HyperSolar's two rows of four facade materials can each expose two live
   chunk runs. Sixteen tiny span descriptors preserve transform-once replay
   for all eight materials; this is bookkeeping only, not retained geometry. */
#define GLDC_CAPTURE_SLOTS 16

typedef struct {
    PolyList* list;
    uint32_t  start;   /* index of the first captured vertex in list->vector */
    uint32_t  count;
} CapturedSpan;

static CapturedSpan CAPTURED_SPANS[GLDC_CAPTURE_SLOTS];
static int CAPTURE_PENDING = -1;

void APIENTRY glKosCaptureArrays(GLuint slot) {
    if(slot < GLDC_CAPTURE_SLOTS) {
        CAPTURE_PENDING = (int) slot;
    }
}

void _glInvalidateCapturedArrays(void) {
    for(int i = 0; i < GLDC_CAPTURE_SLOTS; ++i) {
        CAPTURED_SPANS[i].count = 0;
    }
    CAPTURE_PENDING = -1;
}

/* A draw attempt that emits nothing must not leave a pending capture armed:
   the arm would latch onto the NEXT unrelated draw and the paired replay
   would clone foreign geometry (AUD-001-OPA-01). Cancelling clears the SLOT
   too, so that replay becomes a clean no-op instead. Called only on no-op /
   error exits — never on a draw that emits vertices, so the hot path never
   pays for it. NOT called when a fused lane falls back to glDrawArrays: the
   fallback performs (and captures) the real draw. The TA-sprite lanes are
   outside the capture system entirely — they neither consume nor cancel the
   arm; only vertex-vector draws do. */
static void _glCancelPendingCapture(void) {
    if(CAPTURE_PENDING >= 0) {
        CAPTURED_SPANS[CAPTURE_PENDING].count = 0;
        CAPTURE_PENDING = -1;
    }
}

/* The fused writer raw-reinterprets client pointers (uint32 color reads,
   float position reads): anything but the fast-path layout with 4-aligned
   pointers/strides is an SH4 address-error exception, not a slow path
   (AUD-001-OPA-02). Mirrors the checks the PUC and holo lanes already do. */
GL_FORCE_INLINE GLboolean _glFusedLaneCompatible(void) {
    return ATTRIB_LIST.fast_path &&
           ((((uintptr_t) ATTRIB_LIST.vertex.ptr) | ATTRIB_LIST.vertex.stride |
             ((uintptr_t) ATTRIB_LIST.uv.ptr) | ATTRIB_LIST.uv.stride |
             ((uintptr_t) ATTRIB_LIST.colour.ptr) | ATTRIB_LIST.colour.stride) & 3) == 0;
}

/* One-shot diagnostic for the TA-sprite entry points: outside their contract
   they emit nothing, and a silent drop would read as "the city lights
   vanished" with no clue (AUD-001-OPA-10). */
static void _glSpriteLaneDropWarn(void) {
    static GLboolean warned = GL_FALSE;
    if(!warned) {
        warned = GL_TRUE;
        fprintf(stderr, "[GLDC] sprite draw dropped: TNL effects or immediate mode active\n");
    }
}

/* City window replay's hot case is not a plain clone: every record is copied,
   recolored to a constant and polygon-offset in succession. The old sequence
   made three complete passes over the destination (shz copy, color patch,
   offset patch). Fuse that exact combination into one cache-line write. The
   two simpler API cases retain the proven shz_memcpy32 path below. */
GL_FORCE_INLINE void _glReplayCopyColorOffset(
        Vertex* dst, const Vertex* src, GLuint count,
        const GLubyte* bgra, float offset_inv) {
    const uint32_t color =
        (uint32_t)bgra[0] |
        ((uint32_t)bgra[1] << 8) |
        ((uint32_t)bgra[2] << 16) |
        ((uint32_t)bgra[3] << 24);
    Vertex* const end = dst + count;
    for(; dst < end; ++dst, ++src) {
        PREFETCH(src + 2);
        VERTEX_CACHE_ALLOC(dst);
        dst->flags = src->flags;
        if(src->w != 1.0f) {
            dst->xyz[0] = src->xyz[0] * offset_inv;
            dst->xyz[1] = src->xyz[1] * offset_inv;
            dst->w      = src->w      * offset_inv;
        } else {
            dst->xyz[0] = src->xyz[0];
            dst->xyz[1] = src->xyz[1];
            dst->w      = src->w;
        }
        dst->xyz[2] = src->xyz[2];
        dst->uv[0] = src->uv[0];
        dst->uv[1] = src->uv[1];
        *((uint32_t*) dst->bgra) = color;
    }
}

void APIENTRY glKosReplayArrays(GLuint slot, const GLubyte* bgra) {
    if(slot >= GLDC_CAPTURE_SLOTS) return;

    CapturedSpan* c = &CAPTURED_SPANS[slot];
    if(!c->count || !c->list) return;

    PolyList* out = _glActivePolyList();
    const uint32_t vec = aligned_vector_size(&out->vector);
    const GLboolean header_required = !out->header_emitted || _glGPUStateIsDirty();

    aligned_vector_extend(&out->vector, c->count + (header_required ? 1 : 0));

    if(header_required) {
        apply_poly_header((PolyHeader*) aligned_vector_at(&out->vector, vec), GL_FALSE, out, 0);
        _glGPUStateMarkClean();
        out->header_emitted = GL_TRUE;
    }

    /* Resolve source AFTER the extend: a same-list replay would have realloc'd it. */
    Vertex* src = (Vertex*) aligned_vector_at(&c->list->vector, c->start);
    Vertex* dst = (Vertex*) aligned_vector_at(&out->vector, vec + (header_required ? 1 : 0));
    const GLint radial_mode = _glRadialVertexFog()->mode;
    const GLboolean radial_attenuate =
        radial_mode == GL_KOS_VERTEX_FOG_ATTENUATE_ALPHA;
    const GLboolean radial_blend =
        radial_mode == GL_KOS_VERTEX_FOG_BLEND ||
        radial_mode == GL_KOS_VERTEX_FOG_BLEND_PRECOMPUTED;
    if(bgra && _glPolygonOffsetMul != 1.0f &&
       !radial_attenuate && !radial_blend) {
        /* This fused replay path performs the offset bake itself and returns
           before _glBakePolygonOffset(), so account for it at this choke. */
        GLDC_STAT_ADD(polygon_offset_vertices, c->count);
        _glReplayCopyColorOffset(dst, src, c->count, bgra,
                                 1.0f / _glPolygonOffsetMul);
        return;
    }
#ifdef USE_SH4ZAM
    /* Both sides are 32-byte-aligned Vertex records in aligned_vector storage
       and the size is a multiple of 32: shz_memcpy32's exact contract. Cached
       RAM destination, so the non-SQ variant. */
    shz_memcpy32(dst, src, c->count * sizeof(Vertex));
#else
    memcpy(dst, src, c->count * sizeof(Vertex));
#endif

    if(bgra) {   /* constant color override (NULL keeps the captured tints) */
        Vertex* v = dst;
        const Vertex* s = src;
        Vertex* const end = dst + c->count;
        for(; v < end; ++v, ++s) {
            v->bgra[0] = bgra[0];
            v->bgra[1] = bgra[1];
            v->bgra[2] = bgra[2];
            if(radial_attenuate) {
                /* Captured A stores a precomputed fog amount. ATTENUATE's
                   alpha scales that amount, allowing an additive replay to
                   retain a configurable floor while following the same
                   already-resolved curve. Alpha 1 preserves legacy behavior. */
                const unsigned amount =
                    ((unsigned)s->bgra[3] *
                     (unsigned)_glRadialVertexFog()->amount_scale + 127u) / 255u;
                const unsigned keep = 255u - amount;
                v->bgra[3] = (GLubyte)
                    (((unsigned)bgra[3] * keep + 127u) / 255u);
            } else if(radial_blend) {
                /* A captured opaque shell stores its radial coefficient in A.
                   Preserve it while replacing only the replay's RGB. */
                v->bgra[3] = s->bgra[3];
            } else {
                v->bgra[3] = bgra[3];
            }
        }
    }

    /* Same per-draw bake every entry does. PRECONDITION: the capture itself ran
       offset-free (a captured non-identity offset would compound here). */
    _glBakePolygonOffset(dst, c->count);
}


void _glInitSubmissionTarget() {
    SubmissionTarget* target = &SUBMISSION_TARGET;

    target->extras = NULL;
    target->count = 0;
    target->output = NULL;
    target->header_offset = target->start_offset = 0;

    aligned_vector_init(&VERTEX_EXTRAS, sizeof(VertexExtra));
    target->extras = &VERTEX_EXTRAS;
}

GL_FORCE_INLINE GLuint calcFinalVertices(GLenum mode, GLuint count) {
    switch (mode) {
        case GL_POINTS:
            return POINTS_COUNT(count);
        case GL_LINE_LOOP:
            return LINE_LOOP_COUNT(count);
        case GL_LINE_STRIP:
            return LINE_STRIP_COUNT(count);
        case GL_LINES:
            return LINES_COUNT(count);
        case GL_TRIANGLE_FAN:
            return TRIFAN_COUNT(count);
        case GL_QUAD_STRIP:
            return QUADSTRIP_COUNT(count);
    }
    return count;
}

#include "config.h"
#if GLDC_S3_SEGMENTED_OP
void _glS3DrainOP(void);   /* platforms/sh4.c — S3 segmented hot drain */
#endif

GL_FORCE_INLINE void submitVertices(GLenum mode, GLsizei first, GLuint count, GLenum type, const GLvoid* indices) {
    SubmissionTarget* const target = &SUBMISSION_TARGET;
    AlignedVector* const extras = target->extras;

    TRACE();

    /* Do nothing if vertices aren't enabled */
    if(!(ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG)) {
        _glCancelPendingCapture();
        return;
    }
    if(ATTRIB_LIST.dirty) _glUpdateAttributes();

    /* No vertices? Do nothing */
    if(!count) {
        _glCancelPendingCapture();
        return;
    }

    /* Polygons are treated as triangle fans, the only time this would be a
     * problem is if we supported glPolygonMode(..., GL_LINE) but we don't.
     * We optimise the triangle and quad cases.
     */
    if(mode == GL_POLYGON) {
        switch(count) {
            case 2:
                mode = GL_LINES;
            break;
            case 3:
                mode = GL_TRIANGLES;
            break;
            case 4:
                mode = GL_QUADS;
            break;
            default:
                mode = GL_TRIANGLE_FAN;
        }
    }

    /* Degenerate primitive counts (AUD-001-OPA-05): calcFinalVertices()
       returns fewer records than the generators write below these minimums
       (heap overflow / GLuint underflow), and partial trailing primitives
       write flags past the reservation (genTriangles' EOL pass, genQuadStrip's
       pair loop). Per the GL spec excess trailing vertices are IGNORED —
       truncate, and treat a fully-degenerate count as a clean no-op, not a GL
       error. QUADS truncates too — its trailing partial-quad records were
       fully initialized but never EOL-terminated (reviewer F10); a strip has
       no partial primitive but needs 3 vertices to exist at all. */
    switch(mode) {
        case GL_TRIANGLES:
            count -= count % 3;
            if(!count) {
                _glCancelPendingCapture();
                return;
            }
            break;
        case GL_QUADS:
            count &= ~3u;
            if(!count) {
                _glCancelPendingCapture();
                return;
            }
            break;
        case GL_TRIANGLE_STRIP:
            if(count < 3) {
                _glCancelPendingCapture();
                return;
            }
            break;
        case GL_QUAD_STRIP:
            count &= ~1u;
            if(count < 4) {
                _glCancelPendingCapture();
                return;
            }
            break;
        case GL_TRIANGLE_FAN:
            if(count < 3) {
                _glCancelPendingCapture();
                return;
            }
            break;
        case GL_LINES:
        case GL_LINE_STRIP:
            if(count < 2) {
                _glCancelPendingCapture();
                return;
            }
            break;
        default:
            break;
    }

    /* Count only the validated/truncated input the generators will actually
       transform. Disabled arrays, empty draws and discarded partial
       primitives must not inflate the average. */
    GLDC_STAT_INC(submit_vertices_calls);
    GLDC_STAT_ADD(vertices_transformed, count);

    target->output = _glActivePolyList();
    gl_assert(target->output);
    gl_assert(extras);

#if GLDC_S3_SEGMENTED_OP
    /* Leaving the OP list: hot-drain its undrained records to the TA now,
       while they are still cache-resident (S3 — see config.h). */
    if(target->output != _glOpaquePolyList()) _glS3DrainOP();
#endif

    uint32_t vector_size = aligned_vector_size(&target->output->vector);

    GLboolean header_required = !target->output->header_emitted || _glGPUStateIsDirty();

    target->count = calcFinalVertices(mode, count);
    target->header_offset = vector_size;
    target->start_offset = target->header_offset + (header_required ? 1 : 0);

    gl_assert(target->start_offset >= target->header_offset);
    gl_assert(target->count);

    /* Make sure we have enough room for all the "extra" data. The PUC
       generators are the only ones that never touch VERTEX_EXTRAS — mirror
       their dispatch condition in generate() EXACTLY and skip the grow for
       them (AUD-001-OPA-04: the vector otherwise grows to the peak per-draw
       vertex count as pure garbage). TNL effects read extras regardless of
       generator, so they force the resize. Keep in sync with generate(). */
    const GLboolean puc_skips_extras =
        ATTRIB_LIST.fast_path && !indices &&
        (ATTRIB_LIST.enabled & (ST_ENABLED_FLAG | NORMAL_ENABLED_FLAG)) == 0 &&
        (mode == GL_QUADS || mode == GL_TRIANGLES) &&
        !_glTnlEffectsActive();
    if(!puc_skips_extras) {
        aligned_vector_resize(extras, target->count);
    }

    /* Make room for the vertices and header */
    aligned_vector_extend(&target->output->vector, target->count + (header_required));

    if(header_required) {
        apply_poly_header(_glSubmissionTargetHeader(target), GL_FALSE, target->output, 0);
        _glGPUStateMarkClean();
        target->output->header_emitted = GL_TRUE;
    }

    _glTnlLoadMatrix();

    generate(target, mode, first, count, (GLubyte*) indices, type);

    _glApplyRadialVertexFog(target, mode, first, count, indices);

    _glTnlApplyEffects(target);

    _glBakePolygonOffset(_glSubmissionTargetStart(target), target->count);

    if(CAPTURE_PENDING >= 0) {
        CapturedSpan* c = &CAPTURED_SPANS[CAPTURE_PENDING];
        c->list = target->output;
        c->start = target->start_offset;
        c->count = target->count;
        CAPTURE_PENDING = -1;
    }

    // /*
    //    Now, if multitexturing is enabled, we want to send exactly the same vertices again, except:
    //    - We want to enable blending, and send them to the TR list
    //    - We want to set the depth func to GL_EQUAL
    //    - We want to set the second texture ID
    //    - We want to set the uv coordinates to the passed st ones
    // */

    // if(!TEXTURES_ENABLED[1]) {
    //     /* Multitexture actively disabled */
    //     return;
    // }

    // TextureObject* texture1 = _glGetTexture1();

    // /* Multitexture implicitly disabled */
    // if(!texture1 || ((ATTRIB_LIST.enabled & ST_ENABLED_FLAG) != ST_ENABLED_FLAG)) {
    //     /* Multitexture actively disabled */
    //     return;
    // }

    // /* Push back a copy of the list to the transparent poly list, including the header
    //     (hence the + 1)
    // */
    // Vertex* vertex = aligned_vector_push_back(
    //     &_glTransparentPolyList()->vector, (Vertex*) _glSubmissionTargetHeader(target), target->count + 1
    // );

    // gl_assert(vertex);

    // PolyHeader* mtHeader = (PolyHeader*) vertex++;
    // /* Send the buffer again to the transparent list */
    // apply_poly_header(mtHeader, GL_TRUE, _glTransparentPolyList(), 1);

    // /* Replace the UV coordinates with the ST ones */
    // VertexExtra* ve = aligned_vector_at(target->extras, 0);
    // ITERATE(target->count) {
    //     vertex->uv[0] = ve->st[0];
    //     vertex->uv[1] = ve->st[1];
    //     ++vertex;
    //     ++ve;
    // }
}

/* ---- Fused client-array lanes (2026-07-15, the dcmesh model/batch lanes) ----
   A model made of many short strips paid the per-call submitVertices overhead —
   list bookkeeping plus a full XMTRX MVP concat — TIMES the strip count (the
   F22 is 192 strips: 192 matrix loads per frame), and GL_TRIANGLE_STRIP /
   batch GL_TRIANGLES have no PUC dispatch, so every vertex ALSO took the
   generic per-attribute generator (ST/normal zero-fill, byte color copy).
   These entries pay the overhead ONCE for the whole batch and run a PUC-grade
   fused writer: MOVCA line-alloc, input prefetch, verbatim BGRA word copy
   (GL_BGRA client colors are already GLdc vertex order), EOL prebaked.

   NARROW CONTRACT: vertex 3f / uv 2f / color 4ub client arrays, any stride but
   colors 4-byte aligned (word load); no ST/normals. GL lighting and non-identity
   texture/color matrices, or a glBegin in flight, fall back to the general
   glDrawArrays path (the fused writer goes straight to clip space and skips
   _glTnlApplyEffects). */

/* Shared prologue: reserve total verts (+header if needed) on the active list,
   stamp the header, load the matrix ONCE. Returns the first output vertex. */
static Vertex* _glBeginFusedDraw(GLuint total) {
    SubmissionTarget* const target = &SUBMISSION_TARGET;
    target->output = _glActivePolyList();

#if GLDC_S3_SEGMENTED_OP
    if(target->output != _glOpaquePolyList()) _glS3DrainOP();   /* leaving OP: hot-drain */
#endif

    const uint32_t vector_size = aligned_vector_size(&target->output->vector);
    const GLboolean header_required = !target->output->header_emitted || _glGPUStateIsDirty();

    target->count = total;
    target->header_offset = vector_size;
    target->start_offset = target->header_offset + (header_required ? 1 : 0);

    aligned_vector_extend(&target->output->vector, total + (header_required ? 1 : 0));

    if(header_required) {
        apply_poly_header(_glSubmissionTargetHeader(target), GL_FALSE, target->output, 0);
        _glGPUStateMarkClean();
        target->output->header_emitted = GL_TRUE;
    }

    _glTnlLoadMatrix();
    return _glSubmissionTargetStart(target);
}

/* Shared epilogue: offset bake + capture handoff — mirrors submitVertices, so
   glKosCaptureArrays's "next draw" promise holds on these lanes too. */
static void _glEndFusedDraw(void) {
    SubmissionTarget* const target = &SUBMISSION_TARGET;
    _glBakePolygonOffset(_glSubmissionTargetStart(target), target->count);

    if(CAPTURE_PENDING >= 0) {
        CapturedSpan* c = &CAPTURED_SPANS[CAPTURE_PENDING];
        c->list = target->output;
        c->start = target->start_offset;
        c->count = target->count;
        CAPTURE_PENDING = -1;
    }
}

/* The fused P3F/T2F/BGRA writer both lanes share. tris_eol picks the EOL rule
   (GL_FALSE: strip — last vertex only; GL_TRUE: triangle soup — every 3rd) and
   is a compile-time constant at each call site, so inlining folds it away.
   The uv+color path is split out: the NULL tests are loop-invariant, and the
   real callers always provide both. */
GL_FORCE_INLINE Vertex* _glWriteFusedVertices(
        Vertex* it, const GLubyte* pp, const GLubyte* up, const GLubyte* cp,
        GLuint pstride, GLuint ustride, GLuint cstride,
        GLsizei c, GLboolean tris_eol) {
    GLsizei eol_next = 2;   /* index of the next triangle-soup EOL vertex */

    if(up && cp) {
        GLsizei i = 0;
        /* NOTE(2026-07-16): the gold block was tried here and REVERTED — model
           strips average ~6 verts, so this writer runs 2 pairs + a tail per
           call, and the block's schedule never amortizes (measured ply 1.47 ->
           1.52, enm 1.07 -> 1.14). The C pairs are neutral; gold stays on the
           long-run city quad kernel where it measured -0.1ms. */
        /* Two vertices per shot through the dual-FTRV pair (fv4+fv8): the
           second FTRV issues while the first drains, and GCC schedules the
           pair's loads/stores around the block instead of serializing. */
        for(; i + 2 <= c; i += 2, it += 2) {
            PREFETCH(pp + (pstride << 1));
            VERTEX_CACHE_ALLOC(it);
            VERTEX_CACHE_ALLOC(it + 1);
            const float* qa = (const float*) pp;
            const float* qb = (const float*) (pp + pstride);
            TransformVertex2(qa[0], qa[1], qa[2], it->xyz, &it->w,
                             qb[0], qb[1], qb[2], (it + 1)->xyz, &(it + 1)->w);
            it->uv[0] = ((const float*) up)[0];
            it->uv[1] = ((const float*) up)[1];
            (it + 1)->uv[0] = ((const float*) (up + ustride))[0];
            (it + 1)->uv[1] = ((const float*) (up + ustride))[1];
            *((uint32_t*) it->bgra) = *((const uint32_t*) cp);
            *((uint32_t*) (it + 1)->bgra) = *((const uint32_t*) (cp + cstride));
            if(tris_eol) {
                if(i == eol_next) {
                    it->flags = GPU_CMD_VERTEX_EOL;
                    eol_next += 3;
                } else {
                    it->flags = GPU_CMD_VERTEX;
                }
                if(i + 1 == eol_next) {
                    (it + 1)->flags = GPU_CMD_VERTEX_EOL;
                    eol_next += 3;
                } else {
                    (it + 1)->flags = GPU_CMD_VERTEX;
                }
            } else {
                it->flags = GPU_CMD_VERTEX;
                (it + 1)->flags = GPU_CMD_VERTEX;
            }
            pp += pstride << 1;
            up += ustride << 1;
            cp += cstride << 1;
        }
        for(; i < c; ++i, ++it) {   /* odd tail */
            VERTEX_CACHE_ALLOC(it);
            TransformVertex(((const float*) pp)[0], ((const float*) pp)[1],
                            ((const float*) pp)[2], 1.0f, it->xyz, &it->w);
            it->uv[0] = ((const float*) up)[0];
            it->uv[1] = ((const float*) up)[1];
            up += ustride;
            *((uint32_t*) it->bgra) = *((const uint32_t*) cp);
            cp += cstride;
            if(tris_eol && i == eol_next) {
                it->flags = GPU_CMD_VERTEX_EOL;
                eol_next += 3;
            } else {
                it->flags = GPU_CMD_VERTEX;
            }
            pp += pstride;
        }
    } else {
        for(GLsizei i = 0; i < c; ++i, ++it) {
            PREFETCH(pp + (pstride << 1));
            VERTEX_CACHE_ALLOC(it);
            TransformVertex(((const float*) pp)[0], ((const float*) pp)[1],
                            ((const float*) pp)[2], 1.0f, it->xyz, &it->w);
            if(up) {
                it->uv[0] = ((const float*) up)[0];
                it->uv[1] = ((const float*) up)[1];
                up += ustride;
            } else {
                it->uv[0] = 0; it->uv[1] = 0;
            }
            if(cp) {
                *((uint32_t*) it->bgra) = *((const uint32_t*) cp);
                cp += cstride;
            } else {
                *((uint32_t*) it->bgra) = ~0;
            }
            if(tris_eol && i == eol_next) {
                it->flags = GPU_CMD_VERTEX_EOL;
                eol_next += 3;
            } else {
                it->flags = GPU_CMD_VERTEX;
            }
            pp += pstride;
        }
    }
    /* Strip callers have exactly one EOL. Stamping it once removes the
       loop-carried EOL compare/branch from every strip vertex. tris_eol is a
       call-site constant and this writer is force-inlined. */
    if(!tris_eol) (it - 1)->flags = GPU_CMD_VERTEX_EOL;
    return it;
}

/* Public borrowed-input seam for paired adapters. Keep the layout checks in
   the implementation as well as the consumer: a mismatched compiler ABI must
   fail at build time, never become an SH4 alignment exception on hardware. */
typedef char GLKosP3T2BGRASizeMustBe24[
    sizeof(GLKosVertexP3T2BGRA) == 24 ? 1 : -1];
typedef char GLKosP3T2BGRAPositionMustStartAt0[
    offsetof(GLKosVertexP3T2BGRA, x) == 0 ? 1 : -1];
typedef char GLKosP3T2BGRAUvMustStartAt12[
    offsetof(GLKosVertexP3T2BGRA, u) == 12 ? 1 : -1];
typedef char GLKosP3T2BGRAColorMustStartAt20[
    offsetof(GLKosVertexP3T2BGRA, bgra) == 20 ? 1 : -1];
typedef char GLKosStripRangeSizeMustBe8[
    sizeof(GLKosStripRange) == 8 ? 1 : -1];
typedef char GLKosStripRangeFirstMustStartAt0[
    offsetof(GLKosStripRange, first) == 0 ? 1 : -1];
typedef char GLKosStripRangeCountMustStartAt4[
    offsetof(GLKosStripRange, count) == 4 ? 1 : -1];
typedef char GLKosPvrRecordSizeMustBe32[
    sizeof(GLKosPvrRecord) == 32 ? 1 : -1];
typedef char GLKosPvrVertexCommandMustMatch[
    GL_KOS_PVR_CMD_VERTEX == GPU_CMD_VERTEX ? 1 : -1];
typedef char GLKosPvrVertexEolCommandMustMatch[
    GL_KOS_PVR_CMD_VERTEX_EOL == GPU_CMD_VERTEX_EOL ? 1 : -1];

#ifdef _arch_dreamcast
#define GLDC_DEFERRED_P3T2BGRA_CAPACITY 64u
#define GLDC_DEFERRED_P3T2BGRA_STRIP_CAPACITY 4096u

static GLdcDeferredP3T2BGRA __attribute__((aligned(32)))
    DEFERRED_P3T2BGRA[GLDC_DEFERRED_P3T2BGRA_CAPACITY];
static GLKosStripRange __attribute__((aligned(32)))
    DEFERRED_P3T2BGRA_STRIPS[GLDC_DEFERRED_P3T2BGRA_STRIP_CAPACITY];
static GLuint DEFERRED_P3T2BGRA_COUNT;
static GLuint DEFERRED_P3T2BGRA_VERTICES;
static GLuint DEFERRED_P3T2BGRA_STRIP_COUNT;

#define GLDC_PVR_PACKET_SEGMENT_CAPACITY 256u
#define GLDC_PVR_PACKET_RECORD_LIMIT     65536u
#define GLDC_PVR_PACKET_INITIAL_RECORDS  8192u

static AlignedVector PVR_PACKET_RECORDS;
static GLdcPvrPacket PVR_PACKETS[GLDC_PVR_PACKET_SEGMENT_CAPACITY];
static GLuint PVR_PACKET_COUNT;
static GLuint PVR_PACKET_NEXT_TOKEN = 1u;
static GLboolean PVR_PACKET_INITIALIZED;
static struct {
    GLboolean active;
    GLuint token;
    GLuint first_record;
    GLuint capacity;
    GLuint list_size;
    PolyList* list;
} PVR_PACKET_RESERVATION;

const GLdcDeferredP3T2BGRA* _glDeferredP3T2BGRAAt(GLuint index) {
    return index < DEFERRED_P3T2BGRA_COUNT
        ? &DEFERRED_P3T2BGRA[index] : NULL;
}

GLuint _glDeferredP3T2BGRACount(void) {
    return DEFERRED_P3T2BGRA_COUNT;
}

GLuint _glDeferredP3T2BGRAVertexCount(void) {
    return DEFERRED_P3T2BGRA_VERTICES;
}

GLuint _glDeferredP3T2BGRAListCount(const PolyList* list) {
    GLuint count = 0;
    for(GLuint i = 0; i < DEFERRED_P3T2BGRA_COUNT; ++i) {
        if(DEFERRED_P3T2BGRA[i].list == list) ++count;
    }
    return count;
}

GLuint _glDeferredP3T2BGRAListVertexCount(const PolyList* list) {
    GLuint count = 0;
    for(GLuint i = 0; i < DEFERRED_P3T2BGRA_COUNT; ++i) {
        if(DEFERRED_P3T2BGRA[i].list == list)
            count += DEFERRED_P3T2BGRA[i].count;
    }
    return count;
}

void _glResetDeferredP3T2BGRA(void) {
    DEFERRED_P3T2BGRA_COUNT = 0;
    DEFERRED_P3T2BGRA_VERTICES = 0;
    DEFERRED_P3T2BGRA_STRIP_COUNT = 0;
}

static void _glPvrClearPublicReservation(
        GLKosPvrPacketReservation* reservation) {
    if(!reservation) return;
    reservation->vertices = NULL;
    reservation->capacity = 0;
    reservation->token = 0;
}

static void _glPvrCompileCurrentHeader(PolyHeader* header, PolyList* list) {
    PolyContext ctx;
    _glBuildPolyContext(&ctx, list, 0);
    CompilePolyHeader(header, &ctx);
    header->cmd |= 0xC0000;
}

static GLint _glPvrReservationState(void) {
    if(CAPTURE_PENDING >= 0 || IMMEDIATE_MODE_ACTIVE ||
       _glTnlEffectsActive() || _glIsScissorTestEnabled() ||
       _glPolygonOffsetMul != 1.0f ||
       _glRadialVertexFog()->mode != GL_KOS_VERTEX_FOG_OFF) {
        return GL_KOS_PVR_UNSUPPORTED_STATE;
    }
    return GL_KOS_PVR_OK;
}

static GLboolean _glPvrWordFinite(GLuint word) {
    return (word & 0x7f800000u) != 0x7f800000u;
}

static GLboolean _glPvrWordPositiveFinite(GLuint word) {
    return (word & 0x80000000u) == 0u &&
           (word & 0x7fffffffu) != 0u && _glPvrWordFinite(word);
}

static GLboolean _glPvrRecordFinite(const GLKosPvrRecord* record) {
    /* Integer exponent tests remain real checks under GLdc's -ffast-math;
       __builtin_isfinite may be folded away by -ffinite-math-only. */
    return _glPvrWordFinite(record->word[1]) &&
           _glPvrWordFinite(record->word[2]) &&
           _glPvrWordPositiveFinite(record->word[3]) &&
           _glPvrWordFinite(record->word[4]) &&
           _glPvrWordFinite(record->word[5]);
}

static GLint _glPvrValidateVertexStream(
        const GLKosPvrRecord* records, GLuint count) {
    if(!records || count < 3u) return GL_KOS_PVR_INVALID_PACKET;

    GLuint strip_vertices = 0;
    for(GLuint i = 0; i < count; ++i) {
        const GLuint command = records[i].word[0];
        if(command != GPU_CMD_VERTEX && command != GPU_CMD_VERTEX_EOL)
            return GL_KOS_PVR_INVALID_PACKET;
        if(!_glPvrRecordFinite(records + i))
            return GL_KOS_PVR_INVALID_PACKET;
        ++strip_vertices;
        if(command == GPU_CMD_VERTEX_EOL) {
            if(strip_vertices < 3u) return GL_KOS_PVR_INVALID_PACKET;
            strip_vertices = 0;
        }
    }
    return strip_vertices == 0u ? GL_KOS_PVR_OK
                                : GL_KOS_PVR_INVALID_PACKET;
}

static GLboolean _glPvrPolyHeaderMatchesList(
        const GLKosPvrRecord* record, GPUList expected_list) {
    const GLuint command = record->word[0];
    return (command & 0xf0800000u) == 0x80800000u &&
           (command & GPU_TA_CMD_USERCLIP_MASK) == 0u &&
           ((command & GPU_TA_CMD_TYPE_MASK) >> GPU_TA_CMD_TYPE_SHIFT) ==
               (GLuint)expected_list;
}

void _glInitPvrPackets(void) {
    if(PVR_PACKET_INITIALIZED) return;
    aligned_vector_init(&PVR_PACKET_RECORDS, sizeof(GLKosPvrRecord));
    aligned_vector_reserve(
        &PVR_PACKET_RECORDS, GLDC_PVR_PACKET_INITIAL_RECORDS);
    PVR_PACKET_INITIALIZED = GL_TRUE;
    _glResetPvrPackets();
}

void _glShutdownPvrPackets(void) {
    if(!PVR_PACKET_INITIALIZED) return;
    gl_assert(!PVR_PACKET_RESERVATION.active);
    aligned_vector_cleanup(&PVR_PACKET_RECORDS);
    memset(&PVR_PACKET_RESERVATION, 0, sizeof(PVR_PACKET_RESERVATION));
    PVR_PACKET_COUNT = 0;
    PVR_PACKET_INITIALIZED = GL_FALSE;
}

void _glResetPvrPackets(void) {
    if(!PVR_PACKET_INITIALIZED) return;
    gl_assert(!PVR_PACKET_RESERVATION.active);
    if(PVR_PACKET_RESERVATION.active) {
        aligned_vector_resize(
            &PVR_PACKET_RECORDS, PVR_PACKET_RESERVATION.first_record);
    }
    memset(&PVR_PACKET_RESERVATION, 0, sizeof(PVR_PACKET_RESERVATION));
    aligned_vector_clear(&PVR_PACKET_RECORDS);
    PVR_PACKET_COUNT = 0;
}

const GLdcPvrPacket* _glPvrPacketAt(GLuint index) {
    return index < PVR_PACKET_COUNT ? &PVR_PACKETS[index] : NULL;
}

const GLKosPvrRecord* _glPvrPacketRecords(const GLdcPvrPacket* packet) {
    if(!packet || packet->first_record >=
                  aligned_vector_size(&PVR_PACKET_RECORDS) ||
       packet->record_count > aligned_vector_size(&PVR_PACKET_RECORDS) -
                                  packet->first_record) {
        return NULL;
    }
    return (const GLKosPvrRecord*)aligned_vector_at(
        &PVR_PACKET_RECORDS, packet->first_record);
}

GLuint _glPvrPacketCount(void) {
    return PVR_PACKET_COUNT;
}

GLuint _glPvrPacketListCount(const PolyList* list) {
    GLuint count = 0;
    for(GLuint i = 0; i < PVR_PACKET_COUNT; ++i)
        if(PVR_PACKETS[i].list == list) ++count;
    return count;
}

GLuint _glPvrPacketListRecordCount(const PolyList* list) {
    GLuint count = 0;
    for(GLuint i = 0; i < PVR_PACKET_COUNT; ++i)
        if(PVR_PACKETS[i].list == list) count += PVR_PACKETS[i].record_count;
    return count;
}

GLboolean _glPvrPacketExclusiveReady(void) {
    return !PVR_PACKET_RESERVATION.active && CAPTURE_PENDING < 0 &&
           !IMMEDIATE_MODE_ACTIVE && !_glIsScissorTestEnabled();
}

GLint _glPvrValidateExclusiveList(
        const GLKosPvrListPacket* packet, GPUList expected_list) {
    if(!packet) return GL_KOS_PVR_BAD_ARGUMENT;
    if(packet->record_count == 0) return GL_KOS_PVR_OK;
    if(packet->record_count < 4 ||
       packet->record_count > (GLsizei)GLDC_PVR_PACKET_RECORD_LIMIT ||
       !packet->records || ((uintptr_t)packet->records & 31u) != 0) {
        return GL_KOS_PVR_BAD_ARGUMENT;
    }

    GLboolean have_header = GL_FALSE;
    GLboolean header_has_strip = GL_FALSE;
    GLuint strip_vertices = 0;
    for(GLsizei i = 0; i < packet->record_count; ++i) {
        const GLKosPvrRecord* const record = packet->records + i;
        const GLuint command = record->word[0];
        if(_glPvrPolyHeaderMatchesList(record, expected_list)) {
            if(strip_vertices != 0u || (have_header && !header_has_strip))
                return GL_KOS_PVR_INVALID_PACKET;
            have_header = GL_TRUE;
            header_has_strip = GL_FALSE;
            continue;
        }
        if(command != GPU_CMD_VERTEX && command != GPU_CMD_VERTEX_EOL)
            return GL_KOS_PVR_INVALID_PACKET;
        if(!have_header || !_glPvrRecordFinite(record))
            return GL_KOS_PVR_INVALID_PACKET;
        ++strip_vertices;
        if(command == GPU_CMD_VERTEX_EOL) {
            if(strip_vertices < 3u) return GL_KOS_PVR_INVALID_PACKET;
            strip_vertices = 0;
            header_has_strip = GL_TRUE;
        }
    }
    return have_header && header_has_strip && strip_vertices == 0u
        ? GL_KOS_PVR_OK : GL_KOS_PVR_INVALID_PACKET;
}

GL_FORCE_INLINE GLuint _glPvrNextToken(void) {
    GLuint token = PVR_PACKET_NEXT_TOKEN++;
    if(PVR_PACKET_NEXT_TOKEN == 0u) PVR_PACKET_NEXT_TOKEN = 1u;
    if(token == 0u) token = PVR_PACKET_NEXT_TOKEN++;
    return token;
}

/* Publish an already-sized, GLdc-owned packet span into list chronology. The
   strict public transaction and the monolithic typed producer share this one
   descriptor/sentinel epilogue; validation policy stays in their callers. */
GL_FORCE_INLINE void _glPvrPublishPacket(
        GLuint first_record, GLuint record_count, GLuint vertex_count,
        GLuint token, GLboolean has_header, PolyList* list) {
    const GLuint descriptor_index = PVR_PACKET_COUNT;
    GLdcPvrPacket* const packet = &PVR_PACKETS[descriptor_index];
    packet->first_record = first_record;
    packet->record_count = record_count;
    packet->token = token;
    packet->has_header = has_header;
    packet->list = list;

    Vertex* const sentinel = (Vertex*)aligned_vector_extend(&list->vector, 1u);
    uint32_t* const words = (uint32_t*)sentinel;
    words[0] = GLDC_PVR_PACKET_SENTINEL;
    words[1] = descriptor_index;
    words[2] = ~descriptor_index;
    gl_assert(token != 0u);
    words[3] = token;
    words[4] = 0u;
    words[5] = 0u;
    words[6] = 0u;
    words[7] = GLDC_PVR_PACKET_SENTINEL ^ descriptor_index ^ token;

    ++PVR_PACKET_COUNT;
    if(has_header) {
        list->header_emitted = GL_TRUE;
        _glGPUStateMarkClean();
        GLDC_STAT_INC(headers_emitted);
    }
    GLDC_STAT_INC(pvr_packet_commits);
    GLDC_STAT_ADD(pvr_packet_vertices, vertex_count);
}

GLint APIENTRY glKosPvrPacketReserve(
        GLsizei capacity, GLKosPvrPacketReservation* reservation) {
    GLDC_STAT_INC(pvr_packet_reserve_attempts);
    if(!reservation) return GL_KOS_PVR_BAD_ARGUMENT;
    if(!PVR_PACKET_INITIALIZED)
        return GL_KOS_PVR_UNSUPPORTED_STATE;
    if(PVR_PACKET_RESERVATION.active) {
        /* Leave the caller's object untouched: it may be the live handle
           itself, accidentally reused for a nested reservation attempt. */
        GLDC_STAT_INC(pvr_packet_reject_busy);
        return GL_KOS_PVR_BUSY;
    }
    _glPvrClearPublicReservation(reservation);
    if(capacity < 3) return GL_KOS_PVR_BAD_ARGUMENT;
    const GLint state = _glPvrReservationState();
    if(state != GL_KOS_PVR_OK) {
        GLDC_STAT_INC(pvr_packet_reject_state);
        return state;
    }
    const GLuint arena_size = aligned_vector_size(&PVR_PACKET_RECORDS);
    if(PVR_PACKET_COUNT >= GLDC_PVR_PACKET_SEGMENT_CAPACITY ||
       (GLuint)capacity > GLDC_PVR_PACKET_RECORD_LIMIT - 1u ||
       arena_size > GLDC_PVR_PACKET_RECORD_LIMIT - 1u - (GLuint)capacity) {
        GLDC_STAT_INC(pvr_packet_reject_capacity);
        return GL_KOS_PVR_CAPACITY;
    }

    PolyList* const list = _glActivePolyList();
    GLKosPvrRecord* const block = (GLKosPvrRecord*)aligned_vector_extend(
        &PVR_PACKET_RECORDS, 1u + (GLuint)capacity);
    _glPvrCompileCurrentHeader((PolyHeader*)block, list);

    const GLuint token = _glPvrNextToken();
    PVR_PACKET_RESERVATION.active = GL_TRUE;
    PVR_PACKET_RESERVATION.token = token;
    PVR_PACKET_RESERVATION.first_record = arena_size;
    PVR_PACKET_RESERVATION.capacity = (GLuint)capacity;
    PVR_PACKET_RESERVATION.list_size = aligned_vector_size(&list->vector);
    PVR_PACKET_RESERVATION.list = list;

    reservation->vertices = block + 1;
    reservation->capacity = capacity;
    reservation->token = token;
    GLDC_STAT_INC(pvr_packet_reserve_hits);
    return GL_KOS_PVR_OK;
}

static GLboolean _glPvrReservationMatches(
        const GLKosPvrPacketReservation* reservation) {
    if(!reservation || !PVR_PACKET_RESERVATION.active ||
       reservation->token == 0u ||
       reservation->token != PVR_PACKET_RESERVATION.token ||
       reservation->capacity != (GLsizei)PVR_PACKET_RESERVATION.capacity) {
        return GL_FALSE;
    }
    const GLKosPvrRecord* const header =
        (const GLKosPvrRecord*)aligned_vector_at(
            &PVR_PACKET_RECORDS, PVR_PACKET_RESERVATION.first_record);
    return reservation->vertices == header + 1;
}

static GLint _glPvrPacketCommitChecked(
        GLKosPvrPacketReservation* reservation, GLsizei used,
        GLboolean validate_stream) {
    if(!_glPvrReservationMatches(reservation) || used < 3 ||
       used > reservation->capacity) {
        return GL_KOS_PVR_BAD_ARGUMENT;
    }
    if(_glPvrReservationState() != GL_KOS_PVR_OK ||
       _glActivePolyList() != PVR_PACKET_RESERVATION.list ||
       aligned_vector_size(&PVR_PACKET_RESERVATION.list->vector) !=
           PVR_PACKET_RESERVATION.list_size) {
        GLDC_STAT_INC(pvr_packet_reject_state);
        return GL_KOS_PVR_STATE_CHANGED;
    }

    PolyHeader __attribute__((aligned(32))) current_header;
    _glPvrCompileCurrentHeader(&current_header, PVR_PACKET_RESERVATION.list);
    const GLKosPvrRecord* const saved_header =
        (const GLKosPvrRecord*)aligned_vector_at(
            &PVR_PACKET_RECORDS, PVR_PACKET_RESERVATION.first_record);
    if(memcmp(&current_header, saved_header, sizeof(current_header)) != 0) {
        GLDC_STAT_INC(pvr_packet_reject_state);
        return GL_KOS_PVR_STATE_CHANGED;
    }

    if(validate_stream) {
        const GLint validation = _glPvrValidateVertexStream(
            reservation->vertices, (GLuint)used);
        if(validation != GL_KOS_PVR_OK) {
            GLDC_STAT_INC(pvr_packet_reject_validation);
            return validation;
        }
    }

    const GLboolean header_required =
        !PVR_PACKET_RESERVATION.list->header_emitted || _glGPUStateIsDirty();
    aligned_vector_resize(
        &PVR_PACKET_RECORDS,
        PVR_PACKET_RESERVATION.first_record + 1u + (GLuint)used);
    _glPvrPublishPacket(
        PVR_PACKET_RESERVATION.first_record +
            (header_required ? 0u : 1u),
        (GLuint)used + (header_required ? 1u : 0u),
        (GLuint)used, PVR_PACKET_RESERVATION.token, header_required,
        PVR_PACKET_RESERVATION.list);
    memset(&PVR_PACKET_RESERVATION, 0, sizeof(PVR_PACKET_RESERVATION));
    _glPvrClearPublicReservation(reservation);
    return GL_KOS_PVR_OK;
}

GLint APIENTRY glKosPvrPacketCommit(
        GLKosPvrPacketReservation* reservation, GLsizei used) {
    return _glPvrPacketCommitChecked(reservation, used, GL_TRUE);
}

GLint APIENTRY glKosPvrPacketCancel(
        GLKosPvrPacketReservation* reservation) {
    if(!_glPvrReservationMatches(reservation))
        return GL_KOS_PVR_BAD_ARGUMENT;
    aligned_vector_resize(
        &PVR_PACKET_RECORDS, PVR_PACKET_RESERVATION.first_record);
    memset(&PVR_PACKET_RESERVATION, 0, sizeof(PVR_PACKET_RESERVATION));
    _glPvrClearPublicReservation(reservation);
    GLDC_STAT_INC(pvr_packet_cancels);
    return GL_KOS_PVR_OK;
}

typedef struct GLdcPvrInternalBuild {
    PolyList* list;
    Vertex* vertices;
    GLuint first_record;
    GLboolean has_header;
} GLdcPvrInternalBuild;

/* Internal typed producers cannot call back into GL between construction and
   publication. Reserve exactly the records they need and keep the strict
   public transaction completely out of this hot path. Header compilation is
   deferred until a successful build, so a near rejection only rolls back RAM.
   Public reserve/commit/cancel semantics remain unchanged above. */
GL_FORCE_INLINE GLint _glPvrBeginInternalPacket(
        GLsizei capacity, GLdcPvrInternalBuild* build) {
    GLDC_STAT_INC(pvr_packet_reserve_attempts);
    if(!PVR_PACKET_INITIALIZED)
        return GL_KOS_PVR_UNSUPPORTED_STATE;
    if(PVR_PACKET_RESERVATION.active) {
        GLDC_STAT_INC(pvr_packet_reject_busy);
        return GL_KOS_PVR_BUSY;
    }
    if(capacity < 3) return GL_KOS_PVR_BAD_ARGUMENT;

    const GLint state = _glPvrReservationState();
    if(state != GL_KOS_PVR_OK) {
        GLDC_STAT_INC(pvr_packet_reject_state);
        return state;
    }

    PolyList* const list = _glActivePolyList();
    const GLboolean has_header =
        !list->header_emitted || _glGPUStateIsDirty();
    const GLuint header_records = has_header ? 1u : 0u;
    const GLuint arena_size = aligned_vector_size(&PVR_PACKET_RECORDS);
    if(PVR_PACKET_COUNT >= GLDC_PVR_PACKET_SEGMENT_CAPACITY ||
       (GLuint)capacity > GLDC_PVR_PACKET_RECORD_LIMIT - header_records ||
       arena_size > GLDC_PVR_PACKET_RECORD_LIMIT - header_records -
                        (GLuint)capacity) {
        GLDC_STAT_INC(pvr_packet_reject_capacity);
        return GL_KOS_PVR_CAPACITY;
    }

    GLKosPvrRecord* const block = (GLKosPvrRecord*)aligned_vector_extend(
        &PVR_PACKET_RECORDS, header_records + (GLuint)capacity);

    build->list = list;
    build->vertices = (Vertex*)(block + header_records);
    build->first_record = arena_size;
    build->has_header = has_header;
    GLDC_STAT_INC(pvr_packet_reserve_hits);
    return GL_KOS_PVR_OK;
}

GL_FORCE_INLINE void _glPvrCancelInternalPacket(
        const GLdcPvrInternalBuild* build) {
    aligned_vector_resize(&PVR_PACKET_RECORDS, build->first_record);
    GLDC_STAT_INC(pvr_packet_cancels);
}

GL_FORCE_INLINE void _glPvrCommitInternalPacket(
        const GLdcPvrInternalBuild* build, GLuint vertices) {
    const GLuint header_records = build->has_header ? 1u : 0u;
    if(build->has_header) {
        PolyHeader* const header = (PolyHeader*)aligned_vector_at(
            &PVR_PACKET_RECORDS, build->first_record);
        _glPvrCompileCurrentHeader(header, build->list);
    }
    _glPvrPublishPacket(
        build->first_record, header_records + vertices, vertices,
        _glPvrNextToken(), build->has_header, build->list);
}
#endif

#ifndef _arch_dreamcast
GLint APIENTRY glKosPvrPacketReserve(
        GLsizei capacity, GLKosPvrPacketReservation* reservation) {
    (void)capacity;
    if(reservation) {
        reservation->vertices = NULL;
        reservation->capacity = 0;
        reservation->token = 0;
    }
    return GL_KOS_PVR_UNSUPPORTED_STATE;
}

GLint APIENTRY glKosPvrPacketCommit(
        GLKosPvrPacketReservation* reservation, GLsizei used) {
    (void)reservation;
    (void)used;
    return GL_KOS_PVR_UNSUPPORTED_STATE;
}

GLint APIENTRY glKosPvrPacketCancel(
        GLKosPvrPacketReservation* reservation) {
    (void)reservation;
    return GL_KOS_PVR_UNSUPPORTED_STATE;
}
#endif

GLuint APIENTRY glKosGetFastPathCapabilities(void) {
    return GL_KOS_FAST_PATH_CAPABILITIES;
}

/* Deliberately mode-specific link symbols: the public macro names the symbol
   matching the consumer's compile mode, while each archive defines only its
   own mode. This catches configuration skew in both directions at link time. */
#if defined(GLDC_NATIVE_BENCH) && GLDC_NATIVE_BENCH
void APIENTRY glKosRequireNativeBenchArchive1(void) {}
#else
void APIENTRY glKosRequireNativeBenchArchive0(void) {}
#endif

void APIENTRY glKosRequireDeferredP3T2BGRA(void) {}
void APIENTRY glKosRequirePvrPackets(void) {}

#ifdef _arch_dreamcast
GL_FORCE_INLINE GLboolean _glTryQueueInternalFinalP3T2BGRA(
        GLenum mode, const GLKosVertexP3T2BGRA* vertices, GLsizei count,
        GLboolean trusted) {
    const GLboolean triangles =
        mode == GL_TRIANGLES && count >= 3 && count % 3 == 0;
    const GLboolean quads =
        mode == GL_QUADS && count >= 4 && count % 4 == 0;
    if((!triangles && !quads) || !vertices ||
       ((uintptr_t)vertices & 3u) != 0) {
        GLDC_STAT_INC(pvr_typed_fallbacks);
        return GL_FALSE;
    }

    GLdcPvrInternalBuild build;
    if(_glPvrBeginInternalPacket(count, &build) != GL_KOS_PVR_OK) {
        GLDC_STAT_INC(pvr_typed_fallbacks);
        return GL_FALSE;
    }

    _glTnlLoadMatrix();
    const int result = trusted
        ? SceneBuildTrustedFinalP3T2BGRA(
              (unsigned int)mode, vertices, (int)count, build.vertices)
        : SceneBuildFinalP3T2BGRA(
              (unsigned int)mode, vertices, (int)count, build.vertices);
    if(result != SCENE_FINAL_BUILD_OK) {
        if(result & SCENE_FINAL_BUILD_NEAR)
            GLDC_STAT_INC(pvr_typed_near_fallbacks);
        _glPvrCancelInternalPacket(&build);
        GLDC_STAT_INC(pvr_typed_fallbacks);
        return GL_FALSE;
    }

    /* No external GL call exists inside this monolithic transaction, so the
       preflight state/header is still current. The builder owns final-record
       grammar and classification; publish only the completed private span. */
    _glPvrCommitInternalPacket(&build, (GLuint)count);
    GLDC_STAT_INC(pvr_typed_hits);
    GLDC_STAT_INC(submit_vertices_calls);
    GLDC_STAT_ADD(vertices_transformed, (GLuint)count);
    return GL_TRUE;
}
#endif

GLboolean APIENTRY glKosTryQueueFinalInterleavedP3T2BGRA(
        GLenum mode, const GLKosVertexP3T2BGRA* vertices, GLsizei count) {
    TRACE();
    GLDC_STAT_INC(pvr_typed_attempts);

#ifndef _arch_dreamcast
    (void)mode;
    (void)vertices;
    (void)count;
    GLDC_STAT_INC(pvr_typed_fallbacks);
    return GL_FALSE;
#else
    return _glTryQueueInternalFinalP3T2BGRA(
        mode, vertices, count, GL_FALSE);
#endif
}

GLboolean APIENTRY glKosTryQueueTrustedFinalInterleavedP3T2BGRA(
        GLenum mode, const GLKosVertexP3T2BGRA* vertices, GLsizei count) {
    TRACE();
    GLDC_STAT_INC(pvr_typed_attempts);

#ifndef _arch_dreamcast
    (void)mode;
    (void)vertices;
    (void)count;
    GLDC_STAT_INC(pvr_typed_fallbacks);
    return GL_FALSE;
#else
    return _glTryQueueInternalFinalP3T2BGRA(
        mode, vertices, count, GL_TRUE);
#endif
}

#if defined(GLDC_NATIVE_BENCH) && GLDC_NATIVE_BENCH
GLboolean APIENTRY glKosNativeBenchTryQueueTrustedFinalP3T2BGRA(
        GLenum mode, const GLKosVertexP3T2BGRA* vertices, GLsizei count) {
    return glKosTryQueueTrustedFinalInterleavedP3T2BGRA(
        mode, vertices, count);
}
#endif

GLboolean APIENTRY glKosTryDrawInterleavedP3T2BGRA(
        GLenum mode, const GLKosVertexP3T2BGRA* vertices, GLsizei count) {
    TRACE();

    const GLboolean complete_triangles =
        mode == GL_TRIANGLES && count >= 3 && count % 3 == 0;
    const GLboolean complete_quads =
        mode == GL_QUADS && count >= 4 && count % 4 == 0;
    if(!complete_triangles && !complete_quads) {
        GLDC_STAT_INC(interleaved_fallbacks);
        GLDC_STAT_INC(interleaved_fallback_mode_or_count);
        return GL_FALSE;
    }
    if(!vertices || ((uintptr_t)vertices & 3u) != 0) {
        GLDC_STAT_INC(interleaved_fallbacks);
        GLDC_STAT_INC(interleaved_fallback_alignment);
        return GL_FALSE;
    }
    if(IMMEDIATE_MODE_ACTIVE) {
        GLDC_STAT_INC(interleaved_fallbacks);
        GLDC_STAT_INC(interleaved_fallback_immediate);
        return GL_FALSE;
    }
    if(_glTnlEffectsActive()) {
        GLDC_STAT_INC(interleaved_fallbacks);
        GLDC_STAT_INC(interleaved_fallback_tnl);
        return GL_FALSE;
    }

    const GLint radial_mode = _glRadialVertexFog()->mode;
    if(radial_mode != GL_KOS_VERTEX_FOG_OFF &&
       radial_mode != GL_KOS_VERTEX_FOG_BLEND_PRECOMPUTED) {
        GLDC_STAT_INC(interleaved_fallbacks);
        GLDC_STAT_INC(interleaved_fallback_radial_fog);
        return GL_FALSE;
    }

    /* From this point the call is committed. _glBegin/_glEnd preserve GLdc's
       header/list chronology, matrix load, capture handoff and offset bake;
       SceneListSubmit retains the exact near-plane clip/finalize contract. */
    GLDC_STAT_INC(interleaved_hits);
    GLDC_STAT_ADD(interleaved_vertices, (GLuint)count);
    GLDC_STAT_INC(submit_vertices_calls);
    GLDC_STAT_ADD(vertices_transformed, (GLuint)count);

    Vertex* out = _glBeginFusedDraw((GLuint)count);
    const GLubyte* const base = (const GLubyte*)vertices;
    if(mode == GL_TRIANGLES) {
        _glWriteFusedVertices(out,
                              base + offsetof(GLKosVertexP3T2BGRA, x),
                              base + offsetof(GLKosVertexP3T2BGRA, u),
                              base + offsetof(GLKosVertexP3T2BGRA, bgra),
                              sizeof(GLKosVertexP3T2BGRA),
                              sizeof(GLKosVertexP3T2BGRA),
                              sizeof(GLKosVertexP3T2BGRA),
                              count, GL_TRUE);
    } else {
        const PUCQuadInput input = {
            .positions = base + offsetof(GLKosVertexP3T2BGRA, x),
            .uvs = base + offsetof(GLKosVertexP3T2BGRA, u),
            .colors = base + offsetof(GLKosVertexP3T2BGRA, bgra),
            .position_stride = sizeof(GLKosVertexP3T2BGRA),
            .uv_stride = sizeof(GLKosVertexP3T2BGRA),
            .color_stride = sizeof(GLKosVertexP3T2BGRA),
            .positions_are_float = GL_TRUE
        };
        _glWritePUCQuads(&SUBMISSION_TARGET, &input, (GLuint)count);
    }
    _glEndFusedDraw();
    return GL_TRUE;
}

static GLboolean _glTryDeferQuadsP3T2BGRASwapStable(
        const GLKosVertexP3T2BGRA* vertices,
        const GLfloat* positions, const GLfloat* texcoords,
        const GLubyte* colors, const GLubyte* constant_bgra,
        GLsizei count, GLboolean arrays, GLboolean constant_color,
        GLboolean planar) {
    TRACE();
    GLDC_STAT_INC(deferred_quad_attempts);
    if(arrays) GLDC_STAT_INC(deferred_array_attempts);
    if(constant_color) GLDC_STAT_INC(deferred_color_array_attempts);

#define DEFERRED_REJECT(counter) do {             \
        GLDC_STAT_INC(deferred_quad_fallbacks);   \
        if(arrays)                                \
            GLDC_STAT_INC(deferred_array_fallbacks); \
        if(constant_color)                        \
            GLDC_STAT_INC(deferred_color_array_fallbacks); \
        GLDC_STAT_INC(counter);                   \
        return GL_FALSE;                          \
    } while(0)

#ifndef _arch_dreamcast
    (void)vertices;
    (void)positions;
    (void)texcoords;
    (void)colors;
    (void)constant_bgra;
    (void)count;
    (void)planar;
    DEFERRED_REJECT(deferred_reject_disabled);
#else
    /* Every rejection precedes matrix/header/list mutation. In particular,
       leave a pending capture armed so the caller's synchronous fallback is
       still the draw glKosCaptureArrays() promised to capture. */
    if(count < 4 || count % 4 != 0) {
        DEFERRED_REJECT(deferred_reject_mode_or_count);
    }
    if(arrays) {
        if(!positions || !texcoords || !colors ||
           (constant_color && !constant_bgra) ||
           ((((uintptr_t)positions) | ((uintptr_t)texcoords) |
             ((uintptr_t)colors)) & 3u) != 0) {
            DEFERRED_REJECT(deferred_reject_alignment);
        }
    } else if(!vertices || ((uintptr_t)vertices & 3u) != 0) {
        DEFERRED_REJECT(deferred_reject_alignment);
    }
    if(CAPTURE_PENDING >= 0) {
        DEFERRED_REJECT(deferred_reject_capture);
    }
    const GLint radial_mode = _glRadialVertexFog()->mode;
    if((!constant_color && _glActivePolyList() != _glOpaquePolyList() &&
        (!arrays || radial_mode != GL_KOS_VERTEX_FOG_OFF)) ||
       IMMEDIATE_MODE_ACTIVE || _glTnlEffectsActive() ||
       _glIsScissorTestEnabled() ||
       (constant_color ? radial_mode != GL_KOS_VERTEX_FOG_OFF :
        (radial_mode != GL_KOS_VERTEX_FOG_OFF &&
        (!arrays ||
         radial_mode != GL_KOS_VERTEX_FOG_BLEND_PRECOMPUTED)))) {
        DEFERRED_REJECT(deferred_reject_state);
    }
    if(DEFERRED_P3T2BGRA_COUNT >= GLDC_DEFERRED_P3T2BGRA_CAPACITY ||
       (GLuint)count > UINT_MAX - DEFERRED_P3T2BGRA_VERTICES) {
        DEFERRED_REJECT(deferred_reject_capacity);
    }

    PolyList* const out = _glActivePolyList();
    const GLuint descriptor_index = DEFERRED_P3T2BGRA_COUNT;
    GLdcDeferredP3T2BGRA* const descriptor =
        &DEFERRED_P3T2BGRA[descriptor_index];

    /* Snapshot the exact combined viewport/projection/modelview matrix and
       current PVR W-buffer offset before the caller may change either. */
    _glTnlLoadMatrix();
    DownloadMatrix4x4(&descriptor->mvp);
    descriptor->arrays = arrays;
    descriptor->constant_color = constant_color;
    descriptor->constant_bgra = constant_color
        ? ((uint32_t)constant_bgra[0] |
           (uint32_t)constant_bgra[1] << 8 |
           (uint32_t)constant_bgra[2] << 16 |
           (uint32_t)constant_bgra[3] << 24)
        : 0u;
    if(arrays) {
        descriptor->input.arrays.positions = positions;
        descriptor->input.arrays.texcoords = texcoords;
        descriptor->input.arrays.colors = colors;
    } else {
        descriptor->input.interleaved = vertices;
    }
    if(constant_color) {
        GLDC_STAT_INC(deferred_color_array_hits);
        GLDC_STAT_ADD(deferred_color_array_vertices, (GLuint)count);
    }
    descriptor->count = (GLuint)count;
    descriptor->strips = NULL;
    descriptor->strip_count = 0;
    descriptor->polygon_offset_inv = 1.0f / _glPolygonOffsetMul;
    descriptor->primitive = planar
        ? GLDC_DEFERRED_P3T2BGRA_PLANAR_QUADS
        : GLDC_DEFERRED_P3T2BGRA_QUADS;
    descriptor->list = out;

    const GLuint vector_size = aligned_vector_size(&out->vector);
    const GLboolean header_required =
        !out->header_emitted || _glGPUStateIsDirty();
    aligned_vector_extend(&out->vector, 1u + (header_required ? 1u : 0u));

    GLuint sentinel_offset = vector_size;
    if(header_required) {
        PolyHeader* const header =
            (PolyHeader*)aligned_vector_at(&out->vector, vector_size);
        apply_poly_header(header, GL_FALSE, out, 0);
        _glGPUStateMarkClean();
        out->header_emitted = GL_TRUE;
        ++sentinel_offset;
    }

    Vertex* const sentinel =
        (Vertex*)aligned_vector_at(&out->vector, sentinel_offset);
    uint32_t* const words = (uint32_t*)sentinel;
    memset(sentinel, 0, sizeof(*sentinel));
    words[0] = GLDC_DEFERRED_P3T2BGRA_SENTINEL;
    words[1] = descriptor_index;
    words[2] = ~descriptor_index;
    words[7] = GLDC_DEFERRED_P3T2BGRA_SENTINEL ^ descriptor_index;

    ++DEFERRED_P3T2BGRA_COUNT;
    DEFERRED_P3T2BGRA_VERTICES += (GLuint)count;
    GLDC_STAT_INC(deferred_quad_hits);
    GLDC_STAT_ADD(deferred_quad_vertices, (GLuint)count);
    if(arrays) {
        GLDC_STAT_INC(deferred_array_hits);
        GLDC_STAT_ADD(deferred_array_vertices, (GLuint)count);
    }
    GLDC_STAT_INC(submit_vertices_calls);
    GLDC_STAT_ADD(vertices_transformed,
        (GLuint)(planar ? count - count / 4 : count));
    if(_glPolygonOffsetMul != 1.0f) {
        GLDC_STAT_ADD(polygon_offset_vertices, (GLuint)count);
    }
    return GL_TRUE;
#endif
#undef DEFERRED_REJECT
}

GLboolean APIENTRY glKosTryDeferQuadsP3T2BGRASwapStable(
        const GLKosVertexP3T2BGRA* vertices, GLsizei count) {
    return _glTryDeferQuadsP3T2BGRASwapStable(
        vertices, NULL, NULL, NULL, NULL, count,
        GL_FALSE, GL_FALSE, GL_FALSE);
}

GLboolean APIENTRY glKosTryDeferQuadsP3T2BGRAArraysSwapStable(
        const GLfloat* positions, const GLfloat* texcoords,
        const GLubyte* bgra, GLsizei count) {
    return _glTryDeferQuadsP3T2BGRASwapStable(
        NULL, positions, texcoords, bgra, NULL, count,
        GL_TRUE, GL_FALSE, GL_FALSE);
}

GLboolean APIENTRY glKosTryDeferPlanarQuadsP3T2BGRAArraysSwapStable(
        const GLfloat* positions, const GLfloat* texcoords,
        const GLubyte* bgra, GLsizei count) {
    return _glTryDeferQuadsP3T2BGRASwapStable(
        NULL, positions, texcoords, bgra, NULL, count,
        GL_TRUE, GL_FALSE, GL_TRUE);
}

GLboolean APIENTRY glKosTryDeferQuadsP3T2BGRAArraysColorSwapStable(
        const GLfloat* positions, const GLfloat* texcoords,
        const GLubyte* source_bgra, const GLubyte* constant_bgra,
        GLsizei count) {
    return _glTryDeferQuadsP3T2BGRASwapStable(
        NULL, positions, texcoords, source_bgra, constant_bgra,
        count, GL_TRUE, GL_TRUE, GL_FALSE);
}

/* Conservative object-space classifier used before an all-visible
   interleaved try-call commits any list/header state. It mirrors N2's
   drain-time quad guard: both the ordinary and polygon-offset near planes
   must be comfortably visible, and scalar/FTRV cancellation ambiguity falls
   back synchronously. */
#ifdef _arch_dreamcast
GL_FORCE_INLINE GLboolean _glDeferredVertexSafelyVisible(
        const Matrix4x4* mvp, GLfloat offset_inv,
        const GLKosVertexP3T2BGRA* in) {
    const float* const m = *mvp;
    const float x = in->x;
    const float y = in->y;
    const float z0 = in->z;
    const float z = x * m[2] + y * m[6] + z0 * m[10] + m[14];
    const float w = x * m[3] + y * m[7] + z0 * m[11] + m[15];
    const float near_plain = z + w;
    const float near_offset = z + w * offset_inv;
    const float scale = __builtin_fabsf(z) + __builtin_fabsf(w) *
                        (1.0f + __builtin_fabsf(offset_inv)) + 1.0f;
    const float margin = 32.0f * FLT_EPSILON * scale;
    return near_plain > margin && near_offset > margin;
}
#endif

GLboolean APIENTRY glKosTryDeferTrianglesP3T2BGRASwapStable(
        const GLKosVertexP3T2BGRA* vertices, GLsizei count) {
    TRACE();
    GLDC_STAT_INC(deferred_quad_attempts);
    GLDC_STAT_INC(deferred_triangle_attempts);

#define DEFERRED_TRIANGLE_REJECT(counter) do {       \
        GLDC_STAT_INC(deferred_quad_fallbacks);      \
        GLDC_STAT_INC(deferred_triangle_fallbacks);  \
        GLDC_STAT_INC(counter);                      \
        return GL_FALSE;                             \
    } while(0)

#ifndef _arch_dreamcast
    (void)vertices;
    (void)count;
    DEFERRED_TRIANGLE_REJECT(deferred_reject_disabled);
#else
    /* Every rejection precedes list/header/capture mutation. Loading and
       downloading the current MVP refreshes only GLdc's matrix cache; keep
       the candidate matrix local until the complete payload has passed the
       conservative near-plane scan. */
    if(count < 3 || count % 3 != 0) {
        DEFERRED_TRIANGLE_REJECT(deferred_reject_mode_or_count);
    }
    if(!vertices || ((uintptr_t)vertices & 3u) != 0) {
        DEFERRED_TRIANGLE_REJECT(deferred_reject_alignment);
    }
    if(CAPTURE_PENDING >= 0) {
        DEFERRED_TRIANGLE_REJECT(deferred_reject_capture);
    }
    if(_glActivePolyList() != _glOpaquePolyList() ||
       IMMEDIATE_MODE_ACTIVE || _glTnlEffectsActive() ||
       _glIsScissorTestEnabled() ||
       _glRadialVertexFog()->mode != GL_KOS_VERTEX_FOG_OFF) {
        DEFERRED_TRIANGLE_REJECT(deferred_reject_state);
    }
    if(DEFERRED_P3T2BGRA_COUNT >= GLDC_DEFERRED_P3T2BGRA_CAPACITY ||
       (GLuint)count > UINT_MAX - DEFERRED_P3T2BGRA_VERTICES) {
        DEFERRED_TRIANGLE_REJECT(deferred_reject_capacity);
    }

    Matrix4x4 mvp;
    _glTnlLoadMatrix();
    DownloadMatrix4x4(&mvp);
    const GLfloat offset_inv = 1.0f / _glPolygonOffsetMul;
    for(GLsizei i = 0; i < count; ++i) {
        if(!_glDeferredVertexSafelyVisible(&mvp, offset_inv, vertices + i)) {
            GLDC_STAT_INC(deferred_triangle_near_fallbacks);
            GLDC_STAT_INC(deferred_quad_fallbacks);
            GLDC_STAT_INC(deferred_triangle_fallbacks);
            return GL_FALSE;
        }
    }

    const GLuint descriptor_index = DEFERRED_P3T2BGRA_COUNT;
    GLdcDeferredP3T2BGRA* const descriptor =
        &DEFERRED_P3T2BGRA[descriptor_index];
    memcpy(&descriptor->mvp, &mvp, sizeof(mvp));
    descriptor->input.interleaved = vertices;
    descriptor->strips = NULL;
    descriptor->count = (GLuint)count;
    descriptor->strip_count = 0;
    descriptor->polygon_offset_inv = offset_inv;
    descriptor->arrays = GL_FALSE;
    descriptor->constant_color = GL_FALSE;
    descriptor->constant_bgra = 0u;
    descriptor->primitive = GLDC_DEFERRED_P3T2BGRA_TRIANGLES;

    PolyList* const out = _glOpaquePolyList();
    descriptor->list = out;
    const GLuint vector_size = aligned_vector_size(&out->vector);
    const GLboolean header_required =
        !out->header_emitted || _glGPUStateIsDirty();
    aligned_vector_extend(&out->vector, 1u + (header_required ? 1u : 0u));

    GLuint sentinel_offset = vector_size;
    if(header_required) {
        PolyHeader* const header =
            (PolyHeader*)aligned_vector_at(&out->vector, vector_size);
        apply_poly_header(header, GL_FALSE, out, 0);
        _glGPUStateMarkClean();
        out->header_emitted = GL_TRUE;
        ++sentinel_offset;
    }

    Vertex* const sentinel =
        (Vertex*)aligned_vector_at(&out->vector, sentinel_offset);
    uint32_t* const words = (uint32_t*)sentinel;
    memset(sentinel, 0, sizeof(*sentinel));
    words[0] = GLDC_DEFERRED_P3T2BGRA_SENTINEL;
    words[1] = descriptor_index;
    words[2] = ~descriptor_index;
    words[7] = GLDC_DEFERRED_P3T2BGRA_SENTINEL ^ descriptor_index;

    ++DEFERRED_P3T2BGRA_COUNT;
    DEFERRED_P3T2BGRA_VERTICES += (GLuint)count;
    GLDC_STAT_INC(deferred_quad_hits);
    GLDC_STAT_ADD(deferred_quad_vertices, (GLuint)count);
    GLDC_STAT_INC(deferred_triangle_hits);
    GLDC_STAT_ADD(deferred_triangle_vertices, (GLuint)count);
    GLDC_STAT_INC(submit_vertices_calls);
    GLDC_STAT_ADD(vertices_transformed, (GLuint)count);
    if(_glPolygonOffsetMul != 1.0f) {
        GLDC_STAT_ADD(polygon_offset_vertices, (GLuint)count);
    }
    return GL_TRUE;
#endif
#undef DEFERRED_TRIANGLE_REJECT
}

GLboolean APIENTRY glKosTryDeferMultiStripsP3T2BGRASwapStable(
        const GLKosVertexP3T2BGRA* vertices,
        GLsizei vertex_count,
        const GLKosStripRange* strips, GLsizei strip_count) {
    TRACE();
    GLDC_STAT_INC(deferred_quad_attempts);
    GLDC_STAT_INC(deferred_multistrip_attempts);

#define DEFERRED_MULTISTRIP_REJECT(counter) do {       \
        GLDC_STAT_INC(deferred_quad_fallbacks);         \
        GLDC_STAT_INC(deferred_multistrip_fallbacks);  \
        GLDC_STAT_INC(counter);                        \
        return GL_FALSE;                               \
    } while(0)

#ifndef _arch_dreamcast
    (void)vertices;
    (void)vertex_count;
    (void)strips;
    (void)strip_count;
    DEFERRED_MULTISTRIP_REJECT(deferred_reject_disabled);
#else
    /* All rejections precede list/header/capture mutation. The matrix-cache
       refresh used for conservative classification is semantically neutral. */
    if(!vertices || vertex_count < 3 || !strips || strip_count <= 0) {
        DEFERRED_MULTISTRIP_REJECT(deferred_reject_mode_or_count);
    }
    if(((uintptr_t)vertices & 3u) != 0) {
        DEFERRED_MULTISTRIP_REJECT(deferred_reject_alignment);
    }
    if(CAPTURE_PENDING >= 0) {
        DEFERRED_MULTISTRIP_REJECT(deferred_reject_capture);
    }
    if(_glActivePolyList() != _glOpaquePolyList() ||
       IMMEDIATE_MODE_ACTIVE || _glTnlEffectsActive() ||
       _glIsScissorTestEnabled() ||
       _glRadialVertexFog()->mode != GL_KOS_VERTEX_FOG_OFF) {
        DEFERRED_MULTISTRIP_REJECT(deferred_reject_state);
    }
    if(DEFERRED_P3T2BGRA_COUNT >= GLDC_DEFERRED_P3T2BGRA_CAPACITY ||
       (GLuint)strip_count >
           GLDC_DEFERRED_P3T2BGRA_STRIP_CAPACITY -
               DEFERRED_P3T2BGRA_STRIP_COUNT) {
        DEFERRED_MULTISTRIP_REJECT(deferred_reject_capacity);
    }

    GLuint total = 0;
    for(GLsizei s = 0; s < strip_count; ++s) {
        const GLuint first = strips[s].first;
        const GLuint count = strips[s].count;
        if(count < 3u || first >= (GLuint)vertex_count ||
           count > (GLuint)vertex_count - first ||
           total > (GLuint)INT_MAX - count) {
            DEFERRED_MULTISTRIP_REJECT(deferred_reject_mode_or_count);
        }
        total += count;
    }
    if(total > UINT_MAX - DEFERRED_P3T2BGRA_VERTICES) {
        DEFERRED_MULTISTRIP_REJECT(deferred_reject_capacity);
    }

    const GLuint descriptor_index = DEFERRED_P3T2BGRA_COUNT;
    GLdcDeferredP3T2BGRA* const descriptor =
        &DEFERRED_P3T2BGRA[descriptor_index];
    _glTnlLoadMatrix();
    DownloadMatrix4x4(&descriptor->mvp);
    const GLfloat offset_inv = 1.0f / _glPolygonOffsetMul;

    /* A try-call cannot discover a near strip after returning GL_TRUE: the
       caller's exact synchronous fallback is no longer available then. Scan
       every referenced vertex now, before copying metadata or touching the
       active list. Immutable payload + snapshotted MVP makes this verdict
       identical at drain. */
    for(GLsizei s = 0; s < strip_count; ++s) {
        const GLKosVertexP3T2BGRA* in = vertices + strips[s].first;
        const GLKosVertexP3T2BGRA* const end = in + strips[s].count;
        for(; in < end; ++in) {
            if(!_glDeferredVertexSafelyVisible(
                    &descriptor->mvp, offset_inv, in)) {
                GLDC_STAT_INC(deferred_multistrip_near_fallbacks);
                GLDC_STAT_INC(deferred_quad_fallbacks);
                GLDC_STAT_INC(deferred_multistrip_fallbacks);
                return GL_FALSE;
            }
        }
    }

    GLKosStripRange* const copied =
        &DEFERRED_P3T2BGRA_STRIPS[DEFERRED_P3T2BGRA_STRIP_COUNT];
    memcpy(copied, strips, (size_t)strip_count * sizeof(*copied));
    DEFERRED_P3T2BGRA_STRIP_COUNT += (GLuint)strip_count;

    descriptor->input.interleaved = vertices;
    descriptor->strips = copied;
    descriptor->count = total;
    descriptor->strip_count = (GLuint)strip_count;
    descriptor->polygon_offset_inv = offset_inv;
    descriptor->arrays = GL_FALSE;
    descriptor->constant_color = GL_FALSE;
    descriptor->constant_bgra = 0u;
    descriptor->primitive = GLDC_DEFERRED_P3T2BGRA_MULTISTRIPS;

    PolyList* const out = _glOpaquePolyList();
    descriptor->list = out;
    const GLuint vector_size = aligned_vector_size(&out->vector);
    const GLboolean header_required =
        !out->header_emitted || _glGPUStateIsDirty();
    aligned_vector_extend(&out->vector, 1u + (header_required ? 1u : 0u));

    GLuint sentinel_offset = vector_size;
    if(header_required) {
        PolyHeader* const header =
            (PolyHeader*)aligned_vector_at(&out->vector, vector_size);
        apply_poly_header(header, GL_FALSE, out, 0);
        _glGPUStateMarkClean();
        out->header_emitted = GL_TRUE;
        ++sentinel_offset;
    }

    Vertex* const sentinel =
        (Vertex*)aligned_vector_at(&out->vector, sentinel_offset);
    uint32_t* const words = (uint32_t*)sentinel;
    memset(sentinel, 0, sizeof(*sentinel));
    words[0] = GLDC_DEFERRED_P3T2BGRA_SENTINEL;
    words[1] = descriptor_index;
    words[2] = ~descriptor_index;
    words[7] = GLDC_DEFERRED_P3T2BGRA_SENTINEL ^ descriptor_index;

    ++DEFERRED_P3T2BGRA_COUNT;
    DEFERRED_P3T2BGRA_VERTICES += total;
    GLDC_STAT_INC(deferred_quad_hits);
    GLDC_STAT_ADD(deferred_quad_vertices, total);
    GLDC_STAT_INC(deferred_multistrip_hits);
    GLDC_STAT_ADD(deferred_multistrip_strips, (GLuint)strip_count);
    GLDC_STAT_ADD(deferred_multistrip_vertices, total);
    GLDC_STAT_INC(submit_vertices_calls);
    GLDC_STAT_ADD(vertices_transformed, total);
    if(_glPolygonOffsetMul != 1.0f) {
        GLDC_STAT_ADD(polygon_offset_vertices, total);
    }
    return GL_TRUE;
#endif
#undef DEFERRED_MULTISTRIP_REJECT
}

#if defined(GLDC_NATIVE_BENCH) && GLDC_NATIVE_BENCH
typedef char GLKosNativeBenchRecordSizeMustBe32[
    sizeof(GLKosNativeBenchRecord) == 32 ? 1 : -1];

GLuint APIENTRY glKosNativeBenchArchiveAbiVersion(void) {
    return GL_KOS_NATIVE_BENCH_ABI_VERSION;
}

static GLint _glNativeBenchMapFinalBuild(int result) {
    if(result & SCENE_FINAL_BUILD_INVALID)
        return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
    if(result & SCENE_FINAL_BUILD_NEAR)
        return GL_KOS_NATIVE_BENCH_NEAR_CLIP;
    return GL_KOS_NATIVE_BENCH_OK;
}

static GLint _glNativeBenchCheck(
        GLenum mode, const GLKosVertexP3T2BGRA* vertices, GLsizei count,
        const GLKosNativeBenchRecord* output) {
    const GLboolean triangles =
        mode == GL_TRIANGLES && count >= 3 && count % 3 == 0;
    const GLboolean quads =
        mode == GL_QUADS && count >= 4 && count % 4 == 0;
    const GLboolean strip = mode == GL_TRIANGLE_STRIP && count >= 3;
    if(!triangles && !quads && !strip)
        return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
    if(!vertices || !output || ((uintptr_t)vertices & 3u) != 0 ||
       ((uintptr_t)output & 31u) != 0) {
        return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
    }

    /* These are the state contracts a direct-final-record lane cannot retain.
       They are explicit rejects, never inferred all-visible promises. */
    if(IMMEDIATE_MODE_ACTIVE || _glTnlEffectsActive() ||
       _glPolygonOffsetMul != 1.0f || CAPTURE_PENDING >= 0 ||
       _glRadialVertexFog()->mode != GL_KOS_VERTEX_FOG_OFF ||
       _glActivePolyList() != _glOpaquePolyList() ||
       _glIsFogEnabled() || _glIsScissorTestEnabled()) {
        return GL_KOS_NATIVE_BENCH_UNSUPPORTED_STATE;
    }
    return GL_KOS_NATIVE_BENCH_OK;
}

static void _glNativeBenchCompileHeaderUnchecked(
        GLKosNativeBenchRecord* header) {
    PolyContext ctx;
    PolyHeader compiled;
    _glBuildPolyContext(&ctx, _glActivePolyList(), 0);
    CompilePolyHeader(&compiled, &ctx);
    compiled.cmd |= 0xC0000;  /* same six-strip header bits */
    memcpy(header, &compiled, sizeof(compiled));
}

GLint APIENTRY glKosNativeBenchCompileHeader(
        GLKosNativeBenchRecord* header) {
    if(!header || ((uintptr_t)header & 31u) != 0) {
        return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
    }
    if(_glActivePolyList() != _glOpaquePolyList() ||
       _glIsFogEnabled() || _glIsScissorTestEnabled() ||
       _glRadialVertexFog()->mode != GL_KOS_VERTEX_FOG_OFF) {
        return GL_KOS_NATIVE_BENCH_UNSUPPORTED_STATE;
    }
    _glNativeBenchCompileHeaderUnchecked(header);
    return GL_KOS_NATIVE_BENCH_OK;
}

GLint APIENTRY glKosNativeBenchBuildP3T2BGRA(
        GLenum mode, const GLKosVertexP3T2BGRA* vertices, GLsizei count,
        GLKosNativeBenchRecord* output) {
    const GLint check = _glNativeBenchCheck(mode, vertices, count, output);
    if(check != GL_KOS_NATIVE_BENCH_OK) return check;

    _glTnlLoadMatrix();
    return SceneNativeBenchBuildP3T2BGRA(
        (unsigned int)mode, vertices, (int)count, (Vertex*)output);
}

GLint APIENTRY glKosNativeBenchBuildMultiStripsP3T2BGRA(
        const GLKosVertexP3T2BGRA* vertices,
        const GLsizei* counts, GLsizei strip_count,
        GLKosNativeBenchRecord* output) {
    if(!counts || strip_count <= 0) {
        return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
    }

    GLsizei total = 0;
    for(GLsizei s = 0; s < strip_count; ++s) {
        if(counts[s] < 3 || total > INT_MAX - counts[s]) {
            return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
        }
        total += counts[s];
    }
    const GLint check = _glNativeBenchCheck(
        GL_TRIANGLE_STRIP, vertices, total, output);
    if(check != GL_KOS_NATIVE_BENCH_OK) return check;

    _glTnlLoadMatrix();
    GLsizei first = 0;
    GLboolean all_visible = GL_TRUE;
    for(GLsizei s = 0; s < strip_count; ++s) {
        const GLint result = SceneNativeBenchBuildP3T2BGRA(
            GL_TRIANGLE_STRIP, vertices + first, counts[s],
            (Vertex*)output + first);
        first += counts[s];
        if(result == GL_KOS_NATIVE_BENCH_NEAR_CLIP) {
            all_visible = GL_FALSE;
        } else if(result != GL_KOS_NATIVE_BENCH_OK) {
            return result;
        }
    }
    return all_visible ? GL_KOS_NATIVE_BENCH_OK
                       : GL_KOS_NATIVE_BENCH_NEAR_CLIP;
}

GLint APIENTRY glKosNativeBenchValidateP3T2BGRA(
        GLenum mode, const GLKosVertexP3T2BGRA* vertices, GLsizei count,
        GLKosNativeBenchRecord* candidate,
        GLKosNativeBenchRecord* classic,
        GLuint* mismatch_word) {
    if(mismatch_word) *mismatch_word = 0;
    GLint check = _glNativeBenchCheck(mode, vertices, count, candidate);
    if(check != GL_KOS_NATIVE_BENCH_OK) return check;
    if(!classic || ((uintptr_t)classic & 31u) != 0 || classic == candidate) {
        return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
    }

    _glTnlLoadMatrix();
    check = SceneNativeBenchBuildP3T2BGRA(
        (unsigned int)mode, vertices, (int)count,
        (Vertex*)candidate);
    if(check != GL_KOS_NATIVE_BENCH_OK) return check;

    _glTnlLoadMatrix();
    if(mode == GL_QUADS) {
        /* Production PUC quads consume source 0,1,2,3 but write PVR strip
           order 0,1,3,2 with source 2 carrying EOL. Reorder the untimed
           classic oracle explicitly; the ordinary strip writer otherwise
           preserves source order and would compare the wrong packet. */
        Vertex* out = (Vertex*)classic;
        for(GLsizei i = 0; i < count; i += 4) {
            GLKosVertexP3T2BGRA ordered[4] __attribute__((aligned(32)));
            ordered[0] = vertices[i];
            ordered[1] = vertices[i + 1];
            ordered[2] = vertices[i + 3];
            ordered[3] = vertices[i + 2];
            const GLubyte* const base = (const GLubyte*)ordered;
            _glWriteFusedVertices(
                out, base + offsetof(GLKosVertexP3T2BGRA, x),
                base + offsetof(GLKosVertexP3T2BGRA, u),
                base + offsetof(GLKosVertexP3T2BGRA, bgra),
                sizeof(GLKosVertexP3T2BGRA), sizeof(GLKosVertexP3T2BGRA),
                sizeof(GLKosVertexP3T2BGRA), 4, GL_FALSE);
            out += 4;
        }
    } else {
        const GLubyte* const base = (const GLubyte*)vertices;
        _glWriteFusedVertices(
            (Vertex*)classic,
            base + offsetof(GLKosVertexP3T2BGRA, x),
            base + offsetof(GLKosVertexP3T2BGRA, u),
            base + offsetof(GLKosVertexP3T2BGRA, bgra),
            sizeof(GLKosVertexP3T2BGRA), sizeof(GLKosVertexP3T2BGRA),
            sizeof(GLKosVertexP3T2BGRA), count,
            mode == GL_TRIANGLES ? GL_TRUE : GL_FALSE);
    }
    check = SceneNativeBenchFinalizeClassic((Vertex*)classic, (int)count);
    if(check != GL_KOS_NATIVE_BENCH_OK) return check;

    const size_t bytes = (size_t)count * sizeof(*candidate);
    const GLuint words = (GLuint)count * 8u;
    for(int kernel = 0; kernel < 3; ++kernel) {
        if(memcmp(candidate, classic, bytes) != 0) {
            const GLubyte* a = (const GLubyte*)candidate;
            const GLubyte* b = (const GLubyte*)classic;
            for(GLuint i = 0; i < words; ++i) {
                GLuint aw, bw;
                memcpy(&aw, a + i * sizeof(GLuint), sizeof(aw));
                memcpy(&bw, b + i * sizeof(GLuint), sizeof(bw));
                if(aw != bw) {
                    if(mismatch_word) *mismatch_word = i;
                    return GL_KOS_NATIVE_BENCH_MISMATCH;
                }
            }
            /* Full memcmp differed but no full word did: impossible for
               32-byte records, retained as a defensive mismatch result. */
            return GL_KOS_NATIVE_BENCH_MISMATCH;
        }

        if(kernel == 0) {
            /* The first pass validates the retained N1 control kernel.  Reuse
               its scratch for checked production N3. */
            _glTnlLoadMatrix();
            check = _glNativeBenchMapFinalBuild(SceneBuildFinalP3T2BGRA(
                (unsigned int)mode, vertices, (int)count,
                (Vertex*)candidate));
            if(check != GL_KOS_NATIVE_BENCH_OK) return check;
        } else if(kernel == 1) {
            /* The third pass validates the trusted A/B writer independently.
               All three kernels must remain byte-exact against the ordinary
               classic finalizer for visible input. */
            _glTnlLoadMatrix();
            check = _glNativeBenchMapFinalBuild(
                SceneBuildTrustedFinalP3T2BGRA(
                    (unsigned int)mode, vertices, (int)count,
                    (Vertex*)candidate));
            if(check != GL_KOS_NATIVE_BENCH_OK) return check;
        }
    }
    if(mismatch_word) *mismatch_word = words;
    return GL_KOS_NATIVE_BENCH_OK;
}

GLint APIENTRY glKosNativeBenchBuildTrianglePacketP3T2BGRA(
        const GLKosVertexP3T2BGRA* vertices, GLsizei count,
        GLKosNativeBenchRecord* packet, GLsizei packet_capacity,
        GLsizei* packet_records) {
    if(packet_records) *packet_records = 0;
    if(!packet_records || packet_capacity <= 0) {
        return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
    }

    const GLint check = _glNativeBenchCheck(
        GL_TRIANGLES, vertices, count, packet);
    if(check != GL_KOS_NATIVE_BENCH_OK) return check;

    GLKosNativeBenchRecord header;
    _glNativeBenchCompileHeaderUnchecked(&header);
    _glTnlLoadMatrix();

    int records = 0;
    const GLint result = SceneNativeBenchBuildTrianglePacketP3T2BGRA(
        &header, vertices, (int)count, (Vertex*)packet,
        (int)packet_capacity, &records);
    *packet_records = (GLsizei)records;
    return result;
}

GLint APIENTRY glKosNativeBenchSubmitP3T2BGRAAllVisible(
        GLenum mode, const GLKosVertexP3T2BGRA* vertices, GLsizei count) {
    GLKosNativeBenchRecord header;
    const GLint check = _glNativeBenchCheck(
        mode, vertices, count, &header);
    if(check != GL_KOS_NATIVE_BENCH_OK) return check;

    _glNativeBenchCompileHeaderUnchecked(&header);
    _glTnlLoadMatrix();
    return SceneNativeBenchSubmitP3T2BGRAAllVisible(
        &header, (unsigned int)mode, vertices, NULL, 0, (int)count);
}

GLint APIENTRY glKosNativeBenchSubmitMultiStripsP3T2BGRAAllVisible(
        const GLKosVertexP3T2BGRA* vertices,
        const GLsizei* counts, GLsizei strip_count) {
    if(!counts || strip_count <= 0) {
        return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
    }
    GLsizei total = 0;
    for(GLsizei s = 0; s < strip_count; ++s) {
        if(counts[s] < 3 || total > INT_MAX - counts[s]) {
            return GL_KOS_NATIVE_BENCH_BAD_ARGUMENT;
        }
        total += counts[s];
    }

    GLKosNativeBenchRecord header;
    const GLint check = _glNativeBenchCheck(
        GL_TRIANGLE_STRIP, vertices, total, &header);
    if(check != GL_KOS_NATIVE_BENCH_OK) return check;

    _glNativeBenchCompileHeaderUnchecked(&header);
    _glTnlLoadMatrix();
    return SceneNativeBenchSubmitP3T2BGRAAllVisible(
        &header, GL_TRIANGLE_STRIP, vertices, (const int*)counts,
        (int)strip_count, (int)total);
}
#endif

void APIENTRY glKosDrawMultiStrips(const GLint* firsts, const GLsizei* counts, GLsizei n) {
    TRACE();

    if(n <= 0) {
        _glCancelPendingCapture();
        return;
    }
    if(!(ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG)) {
        _glCancelPendingCapture();
        return;
    }
    if(ATTRIB_LIST.dirty) _glUpdateAttributes();

    if(_glTnlEffectsActive() || IMMEDIATE_MODE_ACTIVE || !_glFusedLaneCompatible()) {
        /* Outside the narrow contract: the general path (or its error).
           No capture cancel here — the fallback draws for real and handles
           the pending arm itself. Degenerate strips are skipped so their
           no-op glDrawArrays cannot cancel the arm meant for a REAL strip
           later in this batch. */
        GLboolean drew = GL_FALSE;
#ifdef GLDC_ENABLE_STATS
        GLuint fallback_strips = 0;
        GLuint fallback_vertices = 0;
#endif
        for(GLsizei s = 0; s < n; ++s) {
            if(counts[s] < 3) continue;
            glDrawArrays(GL_TRIANGLE_STRIP, firsts[s], counts[s]);
            drew = GL_TRUE;
#ifdef GLDC_ENABLE_STATS
            ++fallback_strips;
            fallback_vertices += (GLuint)counts[s];
#endif
        }
        if(!drew) {
            _glCancelPendingCapture();
        } else {
            GLDC_STAT_INC(multistrip_fallbacks);
#ifdef GLDC_ENABLE_STATS
            GLDC_STAT_ADD(strip_count, fallback_strips);
            GLDC_STAT_ADD(strip_vertices_total, fallback_vertices);
#endif
        }
        return;
    }

    /* Degenerate strips are excluded from the reservation AND the emit loop:
       a negative count would otherwise shrink the reservation the positive
       strips then overrun. */
    GLsizei total = 0;
#ifdef GLDC_ENABLE_STATS
    GLuint valid_strips = 0;
#endif
    for(GLsizei i = 0; i < n; ++i) {
        if(counts[i] >= 3) {
            total += counts[i];
#ifdef GLDC_ENABLE_STATS
            ++valid_strips;
#endif
        }
    }
    if(total < 3) {
        _glCancelPendingCapture();
        return;
    }

    GLDC_STAT_INC(submit_vertices_calls);
    GLDC_STAT_ADD(vertices_transformed, (GLuint) total);
    GLDC_STAT_INC(multistrip_hits);
#ifdef GLDC_ENABLE_STATS
    GLDC_STAT_ADD(strip_count, valid_strips);
    GLDC_STAT_ADD(strip_vertices_total, (GLuint)total);
#endif

    const GLuint pstride = ATTRIB_LIST.vertex.stride;
    const GLuint ustride = ATTRIB_LIST.uv.stride;
    const GLuint cstride = ATTRIB_LIST.colour.stride;
    const GLboolean has_uv  = (ATTRIB_LIST.enabled & UV_ENABLED_FLAG) != 0;
    const GLboolean has_col = (ATTRIB_LIST.enabled & DIFFUSE_ENABLED_FLAG) != 0;

    Vertex* it = _glBeginFusedDraw((GLuint) total);

    for(GLsizei s = 0; s < n; ++s) {
        if(counts[s] < 3) continue;   /* excluded from the reservation above */
        const GLubyte* pp = ATTRIB_LIST.vertex.ptr + firsts[s] * pstride;
        const GLubyte* up = has_uv  ? ATTRIB_LIST.uv.ptr     + firsts[s] * ustride : NULL;
        const GLubyte* cp = has_col ? ATTRIB_LIST.colour.ptr + firsts[s] * cstride : NULL;
        it = _glWriteFusedVertices(it, pp, up, cp, pstride, ustride, cstride, counts[s], GL_FALSE);
    }

    _glEndFusedDraw();
}

/* Dynamic hologram lanes (2026-07-29).

   Both keep normal polygon-list records, chronological list placement and the
   ordinary SceneListSubmit near-plane clip path. This is deliberately NOT the
   smaller TA-sprite sidecar: sprites cannot clip a near-crossing quad and would
   make a facade/crown disappear as the camera passes it.

   The first entry consumes independent planar parallelograms in GL_QUADS input
   order. It rejects exactly collapsed placeholders before reserving list
   space, transforms only A/B/C, and derives D=A+C-B in homogeneous clip space.
   Four ordinary vertex records are still emitted, so rasterization is
   unchanged.

   The second entry consumes chains of adjacent quads (a faceted cylinder
   half-ring). Shared endpoints become one triangle strip: N faces need
   2*(N+1) records instead of 4*N. A UV discontinuity (the atlas wrap seam)
   starts a new strip and duplicates only that endpoint, preserving the exact
   per-face mapping. */

GL_FORCE_INLINE GLboolean _glHoloLaneCompatible(void) {
    const GLuint required = VERTEX_ENABLED_FLAG | UV_ENABLED_FLAG | DIFFUSE_ENABLED_FLAG;
    return ATTRIB_LIST.fast_path &&
           (ATTRIB_LIST.enabled & required) == required &&
           (ATTRIB_LIST.enabled & (ST_ENABLED_FLAG | NORMAL_ENABLED_FLAG)) == 0 &&
           ATTRIB_LIST.vertex.ptr && ATTRIB_LIST.uv.ptr && ATTRIB_LIST.colour.ptr &&
           !_glTnlEffectsActive() && !IMMEDIATE_MODE_ACTIVE;
}

GL_FORCE_INLINE GLsizei _glHoloFallbackQuads(
        const GLint* firsts, const GLsizei* counts, GLsizei n) {
    GLsizei total = 0;
    for(GLsizei s = 0; s < n; ++s) {
        if(counts[s] <= 0) continue;
        glDrawArrays(GL_QUADS, firsts[s], counts[s]);
        total += counts[s];
    }
    return total;
}

GL_FORCE_INLINE GLboolean _glHoloQuadCollapsed(
        const GLubyte* pp, GLuint pstride) {
    const float* a = (const float*) pp;
    const float* b = (const float*) (pp + pstride);
    const float* c = (const float*) (pp + (pstride << 1));
#define GLDC_HOLO_SAME(P, Q) \
    ((P)[0] == (Q)[0] && (P)[1] == (Q)[1] && (P)[2] == (Q)[2])
    /* Hidden slots collapse to one point. The inactive half of a non-wrapping
       marquee collapses its vertical edge (B==C). Both are exact comparisons,
       so no small-but-visible quad is culled by an epsilon heuristic. */
    const GLboolean collapsed =
        GLDC_HOLO_SAME(a, b) || GLDC_HOLO_SAME(b, c) || GLDC_HOLO_SAME(a, c);
#undef GLDC_HOLO_SAME
    return collapsed;
}

GL_FORCE_INLINE void _glHoloCopyUVColor(
        Vertex* d, const GLubyte* up, const GLubyte* cp,
        GLboolean aligned_color) {
    d->uv[0] = ((const float*) up)[0];
    d->uv[1] = ((const float*) up)[1];
    if(aligned_color) {
        *((uint32_t*) d->bgra) = *((const uint32_t*) cp);
    } else {
        d->bgra[0] = cp[0]; d->bgra[1] = cp[1];
        d->bgra[2] = cp[2]; d->bgra[3] = cp[3];
    }
}

GLsizei APIENTRY glKosDrawPlanarQuadsArrays(
        const GLint* firsts, const GLsizei* counts, GLsizei n) {
    TRACE();

    if(n <= 0 || !firsts || !counts) {
        _glCancelPendingCapture();
        return 0;
    }
    if(!(ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG)) {
        _glCancelPendingCapture();
        return 0;
    }
    if(ATTRIB_LIST.dirty) _glUpdateAttributes();

    GLboolean valid_counts = GL_TRUE;
    for(GLsizei s = 0; s < n; ++s) {
        if(counts[s] < 0 || (counts[s] & 3)) {
            valid_counts = GL_FALSE;
            break;
        }
    }
    if(!valid_counts || !_glHoloLaneCompatible()) {
        const GLsizei drew = _glHoloFallbackQuads(firsts, counts, n);
        /* A fallback that emitted nothing must not leave the arm pending. */
        if(drew == 0) {
            _glCancelPendingCapture();
        } else {
            GLDC_STAT_INC(planar_quad_fallbacks);
        }
        return drew;
    }

    const GLuint pstride = ATTRIB_LIST.vertex.stride;
    const GLuint ustride = ATTRIB_LIST.uv.stride;
    const GLuint cstride = ATTRIB_LIST.colour.stride;
    const GLboolean aligned_color =
        ((((uintptr_t) ATTRIB_LIST.colour.ptr) | cstride) & 3) == 0;

    /* Exact prepass: collapsed marquee reservations must not consume list RAM,
       TnL or TA bandwidth. This touches positions only and is cheaper than
       transforming four records that cannot cover a pixel. */
    GLsizei active_quads = 0;
    for(GLsizei s = 0; s < n; ++s) {
        const GLubyte* pp = ATTRIB_LIST.vertex.ptr + firsts[s] * pstride;
        for(GLsizei v = 0; v < counts[s]; v += 4, pp += pstride << 2) {
            if(!_glHoloQuadCollapsed(pp, pstride)) ++active_quads;
        }
    }
    if(active_quads == 0) {
        /* Whole batch collapsed (hidden marquee slots) — emits nothing, the
           live AUD-001-OPA-01 trigger on this lane. */
        _glCancelPendingCapture();
        return 0;
    }

    const GLsizei output_count = active_quads << 2;
    GLDC_STAT_INC(submit_vertices_calls);
    GLDC_STAT_ADD(vertices_transformed, (GLuint)(active_quads * 3));
    GLDC_STAT_INC(planar_quad_hits);

    Vertex* it = _glBeginFusedDraw((GLuint) output_count);
    for(GLsizei s = 0; s < n; ++s) {
        const GLubyte* pp = ATTRIB_LIST.vertex.ptr + firsts[s] * pstride;
        const GLubyte* up = ATTRIB_LIST.uv.ptr     + firsts[s] * ustride;
        const GLubyte* cp = ATTRIB_LIST.colour.ptr + firsts[s] * cstride;

        for(GLsizei v = 0; v < counts[s]; v += 4,
                      pp += pstride << 2, up += ustride << 2, cp += cstride << 2) {
            if(_glHoloQuadCollapsed(pp, pstride)) continue;

            /* Output strip order is source 0,1,3,2. Transform A/B as one
               dual-FTRV pair, transform C, and obtain D from linearity of the
               complete homogeneous MVP transform. */
            Vertex* d0 = it;
            Vertex* d1 = it + 1;
            Vertex* d3 = it + 2;  /* source 3 */
            Vertex* d2 = it + 3;  /* source 2, EOL */
            VERTEX_CACHE_ALLOC(d0);
            VERTEX_CACHE_ALLOC(d1);
            VERTEX_CACHE_ALLOC(d3);
            VERTEX_CACHE_ALLOC(d2);

            const float* a = (const float*) pp;
            const float* b = (const float*) (pp + pstride);
            const float* c = (const float*) (pp + (pstride << 1));
            TransformVertex2(a[0], a[1], a[2], d0->xyz, &d0->w,
                             b[0], b[1], b[2], d1->xyz, &d1->w);
            TransformVertex(c[0], c[1], c[2], 1.0f, d2->xyz, &d2->w);
            d3->xyz[0] = d0->xyz[0] + d2->xyz[0] - d1->xyz[0];
            d3->xyz[1] = d0->xyz[1] + d2->xyz[1] - d1->xyz[1];
            d3->xyz[2] = d0->xyz[2] + d2->xyz[2] - d1->xyz[2];
            d3->w      = d0->w      + d2->w      - d1->w;

            _glHoloCopyUVColor(d0, up, cp, aligned_color);
            _glHoloCopyUVColor(d1, up + ustride, cp + cstride, aligned_color);
            _glHoloCopyUVColor(d2, up + (ustride << 1),
                               cp + (cstride << 1), aligned_color);
            _glHoloCopyUVColor(d3, up + ustride * 3,
                               cp + cstride * 3, aligned_color);
            d0->flags = GPU_CMD_VERTEX;
            d1->flags = GPU_CMD_VERTEX;
            d3->flags = GPU_CMD_VERTEX;
            d2->flags = GPU_CMD_VERTEX_EOL;
            it += 4;
        }
    }

    _glEndFusedDraw();
    return output_count;
}

GL_FORCE_INLINE GLboolean _glHoloUVPairShared(
        const GLubyte* prev_up, const GLubyte* next_up, GLuint ustride) {
    const float* pb = (const float*) prev_up;                 /* previous src 0 */
    const float* pt = (const float*) (prev_up + ustride * 3); /* previous src 3 */
    const float* nb = (const float*) (next_up + ustride);     /* next src 1 */
    const float* nt = (const float*) (next_up + (ustride << 1)); /* next src 2 */
    return pb[0] == nb[0] && pb[1] == nb[1] &&
           pt[0] == nt[0] && pt[1] == nt[1];
}

GL_FORCE_INLINE Vertex* _glHoloWriteEndpointPair(
        Vertex* it,
        const GLubyte* bottom_pp, const GLubyte* top_pp,
        const GLubyte* bottom_up, const GLubyte* top_up,
        const GLubyte* bottom_cp, const GLubyte* top_cp,
        GLboolean aligned_color) {
    VERTEX_CACHE_ALLOC(it);
    VERTEX_CACHE_ALLOC(it + 1);
    const float* b = (const float*) bottom_pp;
    const float* t = (const float*) top_pp;
    TransformVertex2(b[0], b[1], b[2], it->xyz, &it->w,
                     t[0], t[1], t[2], (it + 1)->xyz, &(it + 1)->w);
    _glHoloCopyUVColor(it, bottom_up, bottom_cp, aligned_color);
    _glHoloCopyUVColor(it + 1, top_up, top_cp, aligned_color);
    it->flags = GPU_CMD_VERTEX;
    (it + 1)->flags = GPU_CMD_VERTEX;
    return it + 2;
}

GL_FORCE_INLINE Vertex* _glHoloDuplicateEndpointPair(
        Vertex* it,
        const GLubyte* bottom_up, const GLubyte* top_up,
        const GLubyte* bottom_cp, const GLubyte* top_cp,
        GLboolean aligned_color) {
    const Vertex* old_bottom = it - 2;
    const Vertex* old_top = it - 1;
    VERTEX_CACHE_ALLOC(it);
    VERTEX_CACHE_ALLOC(it + 1);
    it->xyz[0] = old_bottom->xyz[0];
    it->xyz[1] = old_bottom->xyz[1];
    it->xyz[2] = old_bottom->xyz[2];
    it->w = old_bottom->w;
    (it + 1)->xyz[0] = old_top->xyz[0];
    (it + 1)->xyz[1] = old_top->xyz[1];
    (it + 1)->xyz[2] = old_top->xyz[2];
    (it + 1)->w = old_top->w;
    _glHoloCopyUVColor(it, bottom_up, bottom_cp, aligned_color);
    _glHoloCopyUVColor(it + 1, top_up, top_cp, aligned_color);
    it->flags = GPU_CMD_VERTEX;
    (it + 1)->flags = GPU_CMD_VERTEX;
    return it + 2;
}

GLsizei APIENTRY glKosDrawQuadStripsArrays(
        const GLint* firsts, const GLsizei* counts, GLsizei n) {
    TRACE();

    if(n <= 0 || !firsts || !counts) {
        _glCancelPendingCapture();
        return 0;
    }
    if(!(ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG)) {
        _glCancelPendingCapture();
        return 0;
    }
    if(ATTRIB_LIST.dirty) _glUpdateAttributes();

    GLboolean valid_counts = GL_TRUE;
    for(GLsizei s = 0; s < n; ++s) {
        if(counts[s] < 4 || (counts[s] & 3)) {
            valid_counts = GL_FALSE;
            break;
        }
    }
    if(!valid_counts || !_glHoloLaneCompatible()) {
        const GLsizei drew = _glHoloFallbackQuads(firsts, counts, n);
        /* A fallback that emitted nothing must not leave the arm pending. */
        if(drew == 0) {
            _glCancelPendingCapture();
        } else {
            GLDC_STAT_INC(quad_strip_fallbacks);
        }
        return drew;
    }

    const GLuint pstride = ATTRIB_LIST.vertex.stride;
    const GLuint ustride = ATTRIB_LIST.uv.stride;
    const GLuint cstride = ATTRIB_LIST.colour.stride;
    const GLboolean aligned_color =
        ((((uintptr_t) ATTRIB_LIST.colour.ptr) | cstride) & 3) == 0;

    /* Two endpoints for the first edge, two for each face's far edge, and
       another two whenever the atlas wrap makes that shared geometric edge
       require distinct UVs. */
    GLsizei output_count = 0;
    GLsizei seam_count = 0;
    for(GLsizei s = 0; s < n; ++s) {
        const GLsizei faces = counts[s] >> 2;
        output_count += 2 + faces * 2;
        const GLubyte* up = ATTRIB_LIST.uv.ptr + firsts[s] * ustride;
        for(GLsizei f = 1; f < faces; ++f) {
            if(!_glHoloUVPairShared(up + (f - 1) * (ustride << 2),
                                    up + f * (ustride << 2), ustride)) {
                output_count += 2;
                ++seam_count;
            }
        }
    }
    if(output_count <= 0) {
        _glCancelPendingCapture();
        return 0;
    }

    GLDC_STAT_INC(submit_vertices_calls);
    GLDC_STAT_ADD(vertices_transformed, (GLuint)(output_count - (seam_count << 1)));
    GLDC_STAT_INC(quad_strip_hits);

    Vertex* it = _glBeginFusedDraw((GLuint) output_count);
    for(GLsizei s = 0; s < n; ++s) {
        const GLsizei faces = counts[s] >> 2;
        const GLubyte* pp = ATTRIB_LIST.vertex.ptr + firsts[s] * pstride;
        const GLubyte* up = ATTRIB_LIST.uv.ptr     + firsts[s] * ustride;
        const GLubyte* cp = ATTRIB_LIST.colour.ptr + firsts[s] * cstride;

        /* Current endpoint = source 1 bottom / source 2 top. */
        it = _glHoloWriteEndpointPair(
            it,
            pp + pstride, pp + (pstride << 1),
            up + ustride, up + (ustride << 1),
            cp + cstride, cp + (cstride << 1),
            aligned_color);

        for(GLsizei f = 0; f < faces; ++f) {
            const GLubyte* fp = pp + f * (pstride << 2);
            const GLubyte* fu = up + f * (ustride << 2);
            const GLubyte* fc = cp + f * (cstride << 2);
            if(f > 0 && !_glHoloUVPairShared(
                    up + (f - 1) * (ustride << 2), fu, ustride)) {
                /* End the preceding strip and duplicate this geometric edge
                   with the new face's post-wrap UVs. Its clip coordinates are
                   already the previous endpoint, so copying them avoids a
                   redundant transform. */
                (it - 1)->flags = GPU_CMD_VERTEX_EOL;
                it = _glHoloDuplicateEndpointPair(
                    it,
                    fu + ustride, fu + (ustride << 1),
                    fc + cstride, fc + (cstride << 1),
                    aligned_color);
            }
            /* Next endpoint = source 0 bottom / source 3 top. */
            it = _glHoloWriteEndpointPair(
                it,
                fp, fp + pstride * 3,
                fu, fu + ustride * 3,
                fc, fc + cstride * 3,
                aligned_color);
        }
        (it - 1)->flags = GPU_CMD_VERTEX_EOL;
    }

    _glEndFusedDraw();
    return output_count;
}

/* Triangles sibling of glKosDrawMultiStrips (same contract, same fused writer):
   for warm batch caches that draw pre-expanded triangle soup in one call — the
   enemy lane. EOL lands on every 3rd vertex. */
/* TA sprite quads (2026-07-16, the glow lane): each planar single-color
   parallelogram (D = A+C-B in object space)
   becomes ONE 64-byte sprite record (vs four 32-byte vertex records) with the
   color in a shared header emitted on color change — headers coalesce best
   when the caller quantizes alpha. Transform + perspective divide happen HERE
   (sprites carry screen coordinates), so the records bypass the submit
   finalizer entirely. Sprites have NO clip path: a quad with any corner past
   the near plane is DROPPED whole. Contract: 12 floats per quad (ring order,
   matching the glow scratch), one color word per quad read at colors[q*4]
   (the scratch's 4-equal-words layout), current texture/blend/depth state.
   Records land at the list tail: additive content is order-independent;
   ordinary alpha content is valid only when the caller deliberately wants it
   after every earlier TR family. */
void APIENTRY glKosDrawSpriteQuads(const GLfloat* pos, const GLuint* colors, GLsizei quads) {
    TRACE();

    if(quads <= 0) return;
    if(_glTnlEffectsActive() || IMMEDIATE_MODE_ACTIVE) {
        GLDC_STAT_INC(sprite_lane_drops);
        _glSpriteLaneDropWarn();   /* narrow contract */
        return;
    }

    GLDC_STAT_INC(sprite_lane_hits);
    GLDC_STAT_ADD(sprite_items, (GLuint)quads);
    _glTnlLoadMatrix();
    SceneSpriteQuads(pos, (const uint32_t*) colors, quads);
}

void APIENTRY glKosDrawSpriteCenters(const GLfloat* centers, const GLuint* colors,
                                     GLsizei sprites,
                                     GLfloat ux, GLfloat uy, GLfloat uz,
                                     GLfloat vx, GLfloat vy, GLfloat vz) {
    TRACE();

    if(sprites <= 0) return;
    if(_glTnlEffectsActive() || IMMEDIATE_MODE_ACTIVE) {
        GLDC_STAT_INC(sprite_lane_drops);
        _glSpriteLaneDropWarn();
        return;
    }

    GLDC_STAT_INC(sprite_lane_hits);
    GLDC_STAT_ADD(sprite_items, (GLuint)sprites);
    _glTnlLoadMatrix();
    SceneSpriteCenters(centers, (const uint32_t*) colors, NULL, NULL, sprites,
                       ux, uy, uz, vx, vy, vz);
}

/* Per-sprite half-size and UV rectangle on the center lane. Variable-size,
   screen-facing orb fields keep one center transform per light without making
   the caller expand four object-space corners. */
void APIENTRY glKosDrawSpriteCentersUVRectScale(const GLfloat* centers,
                                                const GLuint* colors,
                                                const GLfloat* half_sizes,
                                                const GLfloat* uv_rects,
                                                GLsizei sprites,
                                                GLfloat ux, GLfloat uy, GLfloat uz,
                                                GLfloat vx, GLfloat vy, GLfloat vz) {
    TRACE();

    if(sprites <= 0) return;
    if(_glTnlEffectsActive() || IMMEDIATE_MODE_ACTIVE) {
        GLDC_STAT_INC(sprite_lane_drops);
        _glSpriteLaneDropWarn();
        return;
    }

    GLDC_STAT_INC(sprite_lane_hits);
    GLDC_STAT_ADD(sprite_items, (GLuint)sprites);
    _glTnlLoadMatrix();
    SceneSpriteCenters(centers, (const uint32_t*) colors, half_sizes, uv_rects, sprites,
                       ux, uy, uz, vx, vy, vz);
}

/* Camera-plane sibling of the generic center lane. This is deliberately an
   explicit contract rather than a fuzzy transformed-axis test: city lights
   know that their billboard axes are camera-plane vectors, and exact routing
   avoids a per-call/per-sprite heuristic in the hot path. */
void APIENTRY glKosDrawSpriteCentersUVRectScalePlane(const GLfloat* centers,
                                                     const GLuint* colors,
                                                     const GLfloat* half_sizes,
                                                     const GLfloat* uv_rects,
                                                     GLsizei sprites,
                                                     GLfloat ux, GLfloat uy, GLfloat uz,
                                                     GLfloat vx, GLfloat vy, GLfloat vz) {
    TRACE();

    if(sprites <= 0) return;
    if(_glTnlEffectsActive() || IMMEDIATE_MODE_ACTIVE) {
        GLDC_STAT_INC(sprite_lane_drops);
        _glSpriteLaneDropWarn();
        return;
    }

    GLDC_STAT_INC(sprite_lane_hits);
    GLDC_STAT_ADD(sprite_items, (GLuint)sprites);
    _glTnlLoadMatrix();
    SceneSpriteCentersPlane(centers, (const uint32_t*) colors,
                            half_sizes, uv_rects, NULL, sprites, 0, 0.0f,
                            ux, uy, uz, vx, vy, vz);
}

void APIENTRY glKosDrawSpriteCentersUVCellScalePlane(const GLfloat* centers,
                                                     const GLuint* colors,
                                                     const GLfloat* half_sizes,
                                                     const GLubyte* cells,
                                                     GLsizei sprites,
                                                     GLint grid_log2,
                                                     GLfloat inset,
                                                     GLfloat ux, GLfloat uy, GLfloat uz,
                                                     GLfloat vx, GLfloat vy, GLfloat vz) {
    TRACE();

    if(sprites <= 0 || !cells) return;
    if(grid_log2 < 1 || grid_log2 > 3) return;
    if(_glTnlEffectsActive() || IMMEDIATE_MODE_ACTIVE) {
        GLDC_STAT_INC(sprite_lane_drops);
        _glSpriteLaneDropWarn();
        return;
    }

    GLDC_STAT_INC(sprite_lane_hits);
    GLDC_STAT_ADD(sprite_items, (GLuint)sprites);
    _glTnlLoadMatrix();
    SceneSpriteCentersPlane(centers, (const uint32_t*) colors,
                            half_sizes, NULL, cells, sprites,
                            grid_log2, inset,
                            ux, uy, uz, vx, vy, vz);
}

void APIENTRY glKosDrawTrianglesArrays(GLint first, GLsizei count) {
    TRACE();

    /* Trailing partial primitive: the fused writer would emit its records
       without an EOL. Truncate like the public GL path does (codex review). */
    count -= count % 3;
    if(count < 3) {
        _glCancelPendingCapture();
        return;
    }
    if(!(ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG)) {
        _glCancelPendingCapture();
        return;
    }
    if(ATTRIB_LIST.dirty) _glUpdateAttributes();

    if(_glTnlEffectsActive() || IMMEDIATE_MODE_ACTIVE || !_glFusedLaneCompatible()) {
        GLDC_STAT_INC(triangle_array_fallbacks);
        /* Fallback draws for real — it handles the pending arm itself. */
        glDrawArrays(GL_TRIANGLES, first, count);
        return;
    }

    GLDC_STAT_INC(submit_vertices_calls);
    GLDC_STAT_ADD(vertices_transformed, (GLuint) count);
    GLDC_STAT_INC(triangle_array_hits);

    const GLuint pstride = ATTRIB_LIST.vertex.stride;
    const GLuint ustride = ATTRIB_LIST.uv.stride;
    const GLuint cstride = ATTRIB_LIST.colour.stride;
    const GLubyte* pp = ATTRIB_LIST.vertex.ptr + first * pstride;
    const GLubyte* up = (ATTRIB_LIST.enabled & UV_ENABLED_FLAG) ? ATTRIB_LIST.uv.ptr + first * ustride : NULL;
    const GLubyte* cp = (ATTRIB_LIST.enabled & DIFFUSE_ENABLED_FLAG) ? ATTRIB_LIST.colour.ptr + first * cstride : NULL;

    Vertex* it = _glBeginFusedDraw((GLuint) count);
    _glWriteFusedVertices(it, pp, up, cp, pstride, ustride, cstride, count, GL_TRUE);
    _glEndFusedDraw();
}

void APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices) {
    TRACE();
    GLDC_STAT_INC(draw_elements_calls);

    if(_glCheckImmediateModeInactive(__func__)) {
        _glCancelPendingCapture();
        return;
    }

    /* Validate while the count is still signed (AUD-001-OPA-07): negative is
       a GL error, zero a valid no-op; both emit nothing, so neither may leave
       a pending capture armed. submitVertices takes GLuint — a negative
       slipping through would reserve gigabytes. */
    if(count <= 0) {
        if(count < 0) _glKosThrowError(GL_INVALID_VALUE, __func__);
        _glCancelPendingCapture();
        return;
    }

    submitVertices(mode, 0, count, type, indices);
}

void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    TRACE();
    GLDC_STAT_INC(draw_arrays_calls);

    if(_glCheckImmediateModeInactive(__func__)) {
        _glCancelPendingCapture();
        return;
    }

    /* Same signed-count contract as glDrawElements (AUD-001-OPA-07). */
    if(count <= 0) {
        if(count < 0) _glKosThrowError(GL_INVALID_VALUE, __func__);
        _glCancelPendingCapture();
        return;
    }

    submitVertices(mode, first, count, GL_UNSIGNED_INT, NULL);
}

GLuint _glGetActiveClientTexture() {
    return ACTIVE_CLIENT_TEXTURE;
}

void APIENTRY glClientActiveTextureARB(GLenum texture) {
    TRACE();

    if(texture < GL_TEXTURE0_ARB || texture > GL_TEXTURE0_ARB + MAX_GLDC_TEXTURE_UNITS) {
        _glKosThrowError(GL_INVALID_ENUM, __func__);
        return;
    }

    ACTIVE_CLIENT_TEXTURE = (texture == GL_TEXTURE1_ARB) ? 1 : 0;
}
