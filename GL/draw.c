#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>

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
    return *((GLshort*) in);
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

static void generateArraysFastPath_PUC_QUADS(SubmissionTarget* target, const GLsizei first, const GLuint count) {
    if(!(ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG)) return;

    Vertex* const batch_base = (Vertex*) _glSubmissionTargetStart(target);

    /* ---- Fused four-vertex kernel (2026-07-16, the city lane) ----
       The three-pass SoA body below touches every output line three times
       (uv, color, position) with the swizzle recomputed per pass; between
       passes a MOVCA'd line can be evicted and read back. This kernel writes
       each quad's four records COMPLETELY, one MOVCA + full 32-byte fill per
       record, strip order (0,1,3,2) baked into the slot offsets, EOL on the
       last strip record. Requires the full P3F/T2F/C4UB-aligned set — the
       city and glow scratch always are; anything else takes the passes. */
    {
        const GLuint pstride = ATTRIB_LIST.vertex.stride;
        const GLuint ustride = ATTRIB_LIST.uv.stride;
        const GLuint cstride = ATTRIB_LIST.colour.stride;
        const GLubyte* pp = ATTRIB_LIST.vertex.ptr + first * pstride;
        const GLubyte* up = (ATTRIB_LIST.enabled & UV_ENABLED_FLAG) ? ATTRIB_LIST.uv.ptr + first * ustride : NULL;
        const GLubyte* cp = (ATTRIB_LIST.enabled & DIFFUSE_ENABLED_FLAG) ? ATTRIB_LIST.colour.ptr + first * cstride : NULL;

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
                    PUC_Q_VERT(0, GPU_CMD_VERTEX);
                    PUC_Q_VERT(1, GPU_CMD_VERTEX);
                    PUC_Q_VERT(3, GPU_CMD_VERTEX_EOL);
                    PUC_Q_VERT(2, GPU_CMD_VERTEX);
                }
                for(GLuint r = 0; r < (count & 3); ++r, ++it) {
                    PUC_Q_VERT(0, (r == 2) ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX);
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

            for(GLuint q = count >> 2; q--; it += 4) {
                PREFETCH(pp + PUC_PREF_AHEAD);
                PREFETCH(up + PUC_PREF_AHEAD);
                PREFETCH(cp + PUC_PREF_AHEAD);
                PUC_Q_PAIR(0, 1, GPU_CMD_VERTEX, GPU_CMD_VERTEX);
                PUC_Q_PAIR(3, 2, GPU_CMD_VERTEX_EOL, GPU_CMD_VERTEX);   /* src 2 = last strip record */
            }
#undef PUC_Q_PAIR

            /* Partial trailing quad (caller contract violation — the city never
               sends one): the records are already reserved on the list, so they
               must be initialized. Sequential, old flag pattern; the submit
               finalizer's generic path EOL-handles the unterminated span. */
            for(GLuint r = 0; r < (count & 3); ++r, ++it) {
                PUC_Q_VERT(0, (r == 2) ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX);
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
        const int offset = (first + min);
        Vertex* it;
        GLuint stride;
        const GLubyte* ptr;

        /* UV — the FIRST writer of each output vertex: allocate its cache line
           (exactly one line per 32B-aligned Vertex, MOVCA.L) so this write stream
           never reads RAM it is about to overwrite; every field gets filled across
           the UV/color/position passes below. Prefetch the input stream ahead. */
        stride = ATTRIB_LIST.uv.stride;
        ptr = (ATTRIB_LIST.enabled & UV_ENABLED_FLAG) ? ATTRIB_LIST.uv.ptr + (offset * stride) : NULL;
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
        stride = ATTRIB_LIST.colour.stride;
        ptr = (ATTRIB_LIST.enabled & DIFFUSE_ENABLED_FLAG) ? ATTRIB_LIST.colour.ptr + (offset * stride) : NULL;
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
        stride = ATTRIB_LIST.vertex.stride;
        ptr = ATTRIB_LIST.vertex.ptr + (offset * stride);
        it = start;
        for(int_fast32_t i = 0; i < loop; ++i) {
            Vertex* dst = PUC_DST(it, i);
            PREFETCH(ptr + PUC_PREF_AHEAD);
            TransformVertex(((float*) ptr)[0], ((float*) ptr)[1], ((float*) ptr)[2], 1.0f, dst->xyz, &dst->w);
            /* strip-order slot 3 (source vertex 2) carries EOL — no record swap needed */
            dst->flags = (((i & 3) == 2) ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX);
            ptr += stride;
        }
#undef PUC_DST

        /* ST and Normal loops: SKIPPED — not enabled */
    }
}

static void generateArraysFastPath_PUC_TRIS(SubmissionTarget* target, const GLsizei first, const GLuint count) {
    if(!(ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG)) return;

    Vertex* const batch_base = (Vertex*) _glSubmissionTargetStart(target);

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
        for(int_fast32_t i = 0; i < loop; ++i, ++it) {
            PREFETCH(ptr + PUC_PREF_AHEAD);
            TransformVertex(((float*) ptr)[0], ((float*) ptr)[1], ((float*) ptr)[2], 1.0f, it->xyz, &it->w);
            it->flags = ((min + i + 1) % 3 == 0) ? GPU_CMD_VERTEX_EOL : GPU_CMD_VERTEX;
            ptr += stride;
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

        /* Patch C: Specialized P+UV+Color dispatch — skips ST/Normal entirely */
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

    if(_glIsScissorTestEnabled()) {
        ctx.gen.clip_mode = GPU_USERCLIP_INSIDE;
    } else {
        ctx.gen.clip_mode = GPU_USERCLIP_DISABLE;
    }

    if(_glIsFogEnabled()) {
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
#define GLDC_CAPTURE_SLOTS 8

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

void APIENTRY glKosReplayArrays(GLuint slot, const GLubyte* bgra) {
    if(slot >= GLDC_CAPTURE_SLOTS) return;

    CapturedSpan* c = &CAPTURED_SPANS[slot];
    if(!c->count || !c->list) return;

    PolyList* out = _glActivePolyList();
    const uint32_t vec = aligned_vector_size(&out->vector);
    const GLboolean header_required = (vec == 0) || _glGPUStateIsDirty();

    aligned_vector_extend(&out->vector, c->count + (header_required ? 1 : 0));

    if(header_required) {
        apply_poly_header((PolyHeader*) aligned_vector_at(&out->vector, vec), GL_FALSE, out, 0);
        _glGPUStateMarkClean();
    }

    /* Resolve source AFTER the extend: a same-list replay would have realloc'd it. */
    Vertex* src = (Vertex*) aligned_vector_at(&c->list->vector, c->start);
    Vertex* dst = (Vertex*) aligned_vector_at(&out->vector, vec + (header_required ? 1 : 0));
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
        Vertex* const end = dst + c->count;
        for(; v < end; ++v) {
            v->bgra[0] = bgra[0];
            v->bgra[1] = bgra[1];
            v->bgra[2] = bgra[2];
            v->bgra[3] = bgra[3];
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
    GLDC_STAT_INC(submit_vertices_calls);
    GLDC_STAT_ADD(vertices_transformed, count);

    /* Do nothing if vertices aren't enabled */
    if(!(ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG)) return;
    if(ATTRIB_LIST.dirty) _glUpdateAttributes();

    /* No vertices? Do nothing */
    if(!count) return;

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

    target->output = _glActivePolyList();
    gl_assert(target->output);
    gl_assert(extras);

#if GLDC_S3_SEGMENTED_OP
    /* Leaving the OP list: hot-drain its undrained records to the TA now,
       while they are still cache-resident (S3 — see config.h). */
    if(target->output != _glOpaquePolyList()) _glS3DrainOP();
#endif

    uint32_t vector_size = aligned_vector_size(&target->output->vector);

    GLboolean header_required = (vector_size == 0) || _glGPUStateIsDirty();

    target->count = calcFinalVertices(mode, count);
    target->header_offset = vector_size;
    target->start_offset = target->header_offset + (header_required ? 1 : 0);

    gl_assert(target->start_offset >= target->header_offset);
    gl_assert(target->count);

    /* Make sure we have enough room for all the "extra" data */
    aligned_vector_resize(extras, target->count);

    /* Make room for the vertices and header */
    aligned_vector_extend(&target->output->vector, target->count + (header_required));

    if(header_required) {
        apply_poly_header(_glSubmissionTargetHeader(target), GL_FALSE, target->output, 0);
        _glGPUStateMarkClean();
    }

    _glTnlLoadMatrix();

    generate(target, mode, first, count, (GLubyte*) indices, type);

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
    const GLboolean header_required = (vector_size == 0) || _glGPUStateIsDirty();

    target->count = total;
    target->header_offset = vector_size;
    target->start_offset = target->header_offset + (header_required ? 1 : 0);

    aligned_vector_extend(&target->output->vector, total + (header_required ? 1 : 0));

    if(header_required) {
        apply_poly_header(_glSubmissionTargetHeader(target), GL_FALSE, target->output, 0);
        _glGPUStateMarkClean();
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
    GLsizei eol_next = tris_eol ? 2 : c - 1;   /* index of the next EOL vertex */

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
            if(i == eol_next) {
                it->flags = GPU_CMD_VERTEX_EOL;
                eol_next += 3;   /* only reachable again on the tris rule */
            } else {
                it->flags = GPU_CMD_VERTEX;
            }
            if(i + 1 == eol_next) {
                (it + 1)->flags = GPU_CMD_VERTEX_EOL;
                eol_next += 3;
            } else {
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
            if(i == eol_next) {
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
            if(i == eol_next) {
                it->flags = GPU_CMD_VERTEX_EOL;
                eol_next += 3;
            } else {
                it->flags = GPU_CMD_VERTEX;
            }
            pp += pstride;
        }
    }
    return it;
}

void APIENTRY glKosDrawMultiStrips(const GLint* firsts, const GLsizei* counts, GLsizei n) {
    TRACE();

    if(n <= 0) return;
    if(!(ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG)) return;
    if(ATTRIB_LIST.dirty) _glUpdateAttributes();

    if(_glTnlEffectsActive() || IMMEDIATE_MODE_ACTIVE) {
        /* Outside the narrow contract: the general path (or its error). */
        for(GLsizei s = 0; s < n; ++s) {
            glDrawArrays(GL_TRIANGLE_STRIP, firsts[s], counts[s]);
        }
        return;
    }

    GLsizei total = 0;
    for(GLsizei i = 0; i < n; ++i) total += counts[i];
    if(total < 3) return;

    GLDC_STAT_INC(submit_vertices_calls);
    GLDC_STAT_ADD(vertices_transformed, (GLuint) total);

    const GLuint pstride = ATTRIB_LIST.vertex.stride;
    const GLuint ustride = ATTRIB_LIST.uv.stride;
    const GLuint cstride = ATTRIB_LIST.colour.stride;
    const GLboolean has_uv  = (ATTRIB_LIST.enabled & UV_ENABLED_FLAG) != 0;
    const GLboolean has_col = (ATTRIB_LIST.enabled & DIFFUSE_ENABLED_FLAG) != 0;

    Vertex* it = _glBeginFusedDraw((GLuint) total);

    for(GLsizei s = 0; s < n; ++s) {
        const GLubyte* pp = ATTRIB_LIST.vertex.ptr + firsts[s] * pstride;
        const GLubyte* up = has_uv  ? ATTRIB_LIST.uv.ptr     + firsts[s] * ustride : NULL;
        const GLubyte* cp = has_col ? ATTRIB_LIST.colour.ptr + firsts[s] * cstride : NULL;
        it = _glWriteFusedVertices(it, pp, up, cp, pstride, ustride, cstride, counts[s], GL_FALSE);
    }

    _glEndFusedDraw();
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
   (the scratch's 4-equal-words layout), current texture/blend/depth state,
   ADDITIVE/order-free content only (records land at the list tail). */
void APIENTRY glKosDrawSpriteQuads(const GLfloat* pos, const GLuint* colors, GLsizei quads) {
    TRACE();

    if(quads <= 0) return;
    if(_glTnlEffectsActive() || IMMEDIATE_MODE_ACTIVE) return;   /* narrow contract */

    _glTnlLoadMatrix();
    SceneSpriteQuads(pos, (const uint32_t*) colors, quads);
}

void APIENTRY glKosDrawSpriteCenters(const GLfloat* centers, const GLuint* colors,
                                     GLsizei sprites,
                                     GLfloat ux, GLfloat uy, GLfloat uz,
                                     GLfloat vx, GLfloat vy, GLfloat vz) {
    TRACE();

    if(sprites <= 0) return;
    if(_glTnlEffectsActive() || IMMEDIATE_MODE_ACTIVE) return;

    _glTnlLoadMatrix();
    SceneSpriteCenters(centers, (const uint32_t*) colors, sprites,
                       ux, uy, uz, vx, vy, vz);
}

void APIENTRY glKosDrawTrianglesArrays(GLint first, GLsizei count) {
    TRACE();

    if(count < 3) return;
    if(!(ATTRIB_LIST.enabled & VERTEX_ENABLED_FLAG)) return;
    if(ATTRIB_LIST.dirty) _glUpdateAttributes();

    if(_glTnlEffectsActive() || IMMEDIATE_MODE_ACTIVE) {
        glDrawArrays(GL_TRIANGLES, first, count);
        return;
    }

    GLDC_STAT_INC(submit_vertices_calls);
    GLDC_STAT_ADD(vertices_transformed, (GLuint) count);

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
        return;
    }

    submitVertices(mode, 0, count, type, indices);
}

void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    TRACE();
    GLDC_STAT_INC(draw_arrays_calls);

    if(_glCheckImmediateModeInactive(__func__)) {
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
