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

/* B2 GOLD-BLOCK quad writer (2026-07-24, HyperSolar perf ledger B2): the city
   PUC_QUADS lane transforms a whole quad per scheduled block — FOUR FTRVs in
   true flight across fv0/fv4/fv8/fv12 (vs the pair's two), quad swizzle
   (0,1,3,2) and EOL baked as fixed record offsets, uv/color moved as GP-word
   copies.
   MEASURED VERDICT (2026-07-24 hardware A/B, first boot correct): bld= 0.54 ->
   0.51-0.53, win= 0.42-0.48 -> 0.40-0.42, city=/fps/late unchanged — the noise
   floor, exactly as the ledger bounded it (the 2k-vert workload is already
   cache-resident; FTRV latency was already mostly hidden by the pair). Per the
   pre-agreed adopt-only-if-clear rule: DEFAULT OFF, dormant and hardware-
   validated beside S3 for a future vertex-volume jump:
       make gldc GLDC_GOLD_BLOCK=1 */
#ifndef GLDC_GOLD_BLOCK
#define GLDC_GOLD_BLOCK 0
#endif

/* [GLDC-T] swap-time telemetry (2026-07-15/31 investigations): flush.c's
   wait/op/pt/tr/fin split + the sh4.c sprite-lane timers (sprcall=). Lives
   here (not flush.c) so BOTH files compile it out together — the sprite
   timers used to run unconditionally (Audit #001 OPA-12). 0 removes the
   timer sampling and the print; the cheap event counters (grow=, hdr/rec)
   still tick. Override without editing: make gldc GLDC_SWAP_TELEMETRY=0
   (if the wrapper forwards it) or flip the default below. */
#ifndef GLDC_SWAP_TELEMETRY
#define GLDC_SWAP_TELEMETRY 1   /* bottleneck hunt 2026-07-31: [GLDC-T] split ON — return to 0 after */
#endif
