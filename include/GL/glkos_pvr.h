#pragma once

/*
 * Expert Dreamcast Tile Accelerator packet API.
 *
 * Ordinary applications should keep using GL/raylib. This boundary exists for
 * renderers which already own final, screen-space 32-byte TA vertex records.
 * Mixed packets remain GLdc-owned and chronological: GLdc compiles the polygon
 * header, owns the backing RAM, and opens/closes the PVR scene and lists.
 */

#include "glkos.h"

__BEGIN_DECLS

#define GL_KOS_PVR_PACKET_ABI_VERSION 1u
#define GL_KOS_PVR_CMD_VERTEX     0xe0000000u
#define GL_KOS_PVR_CMD_VERTEX_EOL 0xf0000000u

/* Final vertex layout for reserve/commit: command, screen X/Y, positive
 * inverse-W depth, U/V, packed BGRA, and the unused offset-color word. The
 * mixed API's GLdc-owned headers keep offset color disabled. */
typedef struct __attribute__((aligned(32))) GLKosPvrRecord {
    GLuint word[8];
} GLKosPvrRecord;

typedef struct GLKosPvrPacketReservation {
    /* Writable final-vertex slots. The GLdc-owned header is intentionally not
     * exposed. This pointer becomes invalid at commit, cancel, swap, RTT flush,
     * or shutdown. */
    GLKosPvrRecord* vertices;
    GLsizei capacity;
    GLuint token;
} GLKosPvrPacketReservation;

enum {
    GL_KOS_PVR_OK = 0,
    GL_KOS_PVR_BAD_ARGUMENT = 1,
    GL_KOS_PVR_UNSUPPORTED_STATE = 2,
    GL_KOS_PVR_BUSY = 3,
    GL_KOS_PVR_CAPACITY = 4,
    GL_KOS_PVR_INVALID_PACKET = 5,
    GL_KOS_PVR_STATE_CHANGED = 6,
    GL_KOS_PVR_QUEUE_NOT_EMPTY = 7,
    GL_KOS_PVR_PVR_ERROR = 8,
    GL_KOS_PVR_NEAR_CLIP = 9
};

/* Reserve GLdc-owned storage for final TA VERTEX/EOL records. Exactly one
 * reservation may be open. No GL/raylib call is permitted between reserve and
 * commit/cancel. Commit validates the command stream, inserts one chronological
 * segment in the current GLdc list. GLdc emits or elides its own header using
 * the same dirty-state chronology as an ordinary draw. A failed commit leaves
 * the reservation open so the caller can inspect/fix it or cancel it. */
GLAPI GLint APIENTRY glKosPvrPacketReserve(
    GLsizei capacity, GLKosPvrPacketReservation* reservation);
GLAPI GLint APIENTRY glKosPvrPacketCommit(
    GLKosPvrPacketReservation* reservation, GLsizei used);
GLAPI GLint APIENTRY glKosPvrPacketCancel(
    GLKosPvrPacketReservation* reservation);

/* Complete list-major command streams for a whole exclusive scene. Each
 * non-empty packet contains one or more polygon headers followed by terminated
 * strips of final vertices; every header must name the matching list and keep
 * user clipping disabled (this V1 grammar has no USERCLIP record). List streams
 * must remain immutable and alive through this synchronous call; invoke it only
 * on the render thread, with no concurrent producer mutation. GLdc still owns
 * the PVR fence, queued fog application, list lifetime, scene finish, its own
 * deferred-free queue, capture invalidation, and frame reset. Raw texture or
 * palette VRAM named by caller-authored headers is not GLdc-owned and must stay
 * live until the next PVR-ready fence after this call. This call rejects any
 * queued ordinary/N2/N3/sprite work. The paired raylib external state barrier
 * must be called before entering this API. */
typedef struct GLKosPvrListPacket {
    const GLKosPvrRecord* records;
    GLsizei record_count;
} GLKosPvrListPacket;

typedef struct GLKosPvrExclusiveScene {
    GLKosPvrListPacket opaque;
    GLKosPvrListPacket punch_through;
    GLKosPvrListPacket translucent;
} GLKosPvrExclusiveScene;

GLAPI GLint APIENTRY glKosPvrSubmitExclusiveScene(
    const GLKosPvrExclusiveScene* scene);

__END_DECLS
