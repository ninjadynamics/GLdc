#pragma once

/* This figure is derived from the needs of Quake 1 */
#define MAX_TEXTURE_COUNT 1088

/* S3 segmented hot drain (2026-07-23, HyperSolar perf ledger A3): when 1, the
   OPAQUE list's buffered records are drained to the TA incrementally at each
   OP -> non-OP draw transition — opportunistically (only when pvr_check_ready
   says the previous render is done) and overlapped with the frame's later CPU
   work — instead of one cold read-back at swap. Writes stay fully buffered
   (capture/replay, near-plane clipping and record layout untouched); the scene
   + OP list open lazily at the first eligible drain and OP stays open until
   swap; PT/TR keep the classic swap-time path.
   MEASURED VERDICT (2026-07-23 hardware A/B): at HyperSolar's current load the
   frame is NOT swap-submission-bound — v2 late frames 188 vs legacy 181 (noise),
   frame time flat. DEFAULT OFF; dormant, hardware-validated machinery for the
   day a scene actually becomes submission-bound:
       make gldc GLDC_S3_SEGMENTED_OP=1 */
#ifndef GLDC_S3_SEGMENTED_OP
#define GLDC_S3_SEGMENTED_OP 0
#endif
