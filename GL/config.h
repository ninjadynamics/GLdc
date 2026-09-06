#pragma once

/* This figure is derived from the needs of Quake 1 */
#define MAX_TEXTURE_COUNT 1088

/* Last-completed-frame telemetry is separate from window aggregates so its
   bounded per-swap bookkeeping can be A/B'd while aggregate timers stay on. */
#ifndef GLDC_SWAP_FRAME_TELEMETRY
#define GLDC_SWAP_FRAME_TELEMETRY 1
#endif
#if GLDC_SWAP_FRAME_TELEMETRY != 0 && GLDC_SWAP_FRAME_TELEMETRY != 1
#error "GLDC_SWAP_FRAME_TELEMETRY must be 0 or 1"
#endif

/* Exact N2 SoA pair preparation experiment: share the polygon-offset guard
   and prepare both independent reciprocals before committing the first SQ.
   Set to 0 for the original per-vertex preparation schedule during hardware
   A/B; transforms, record order, depth and fog expressions are unchanged. */
#ifndef GLDC_N2_ARRAY_PAIR_PREP
#define GLDC_N2_ARRAY_PAIR_PREP 1
#endif

/* Classify the existing bounded SoA run with one scalar loop. Neutral polygon
   offset selects its equivalent single near-plane test once per run. Set to
   0 for the previous per-quad classifier and scheduling during hardware A/B. */
#ifndef GLDC_N2_BATCH_CLASSIFY
#define GLDC_N2_BATCH_CLASSIFY 1
#endif
#if GLDC_N2_BATCH_CLASSIFY != 0 && GLDC_N2_BATCH_CLASSIFY != 1
#error "GLDC_N2_BATCH_CLASSIFY must be 0 or 1"
#endif

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

/* N4 direct vertex-DMA sink (2026-08-24): final OP/PT/TR TA records are
   constructed straight into KOS's double-buffered per-list system-RAM
   buffers. pvr_scene_finish() then launches the list-major DMA chain while
   the CPU starts the next game frame. Capacity is derived from the queued
   scene before it begins and the buffers grow only at a safe TA boundary.

   This is deliberately an opt-in hardware experiment. N2 + F1 remains the
   production route until whole-frame hardware A/B proves that hiding the TA
   transfer outweighs cached-RAM construction and DMA setup:
       make all GLDC_N4_VERTEX_DMA=1 */
#ifndef GLDC_N4_VERTEX_DMA
#define GLDC_N4_VERTEX_DMA 0
#endif

#if GLDC_N4_VERTEX_DMA && !defined(_arch_dreamcast)
#error "GLDC_N4_VERTEX_DMA is a Dreamcast-only platform route"
#endif

/* N2 swap-stable descriptors are now part of every Dreamcast build. S3 owns
   an open OP list mid-frame and cannot preserve descriptor chronology. */
#if defined(_arch_dreamcast) && GLDC_S3_SEGMENTED_OP
#error "Permanent N2 descriptors and GLDC_S3_SEGMENTED_OP cannot be combined"
#endif

/* S3 owns an open hardware list during game-frame construction; N4 instead
   owns complete list-major RAM buffers and starts their DMA at scene finish.
   KOS has no mid-list DMA flush, so the ownership models cannot mix. */
#if defined(_arch_dreamcast) && GLDC_N4_VERTEX_DMA && GLDC_S3_SEGMENTED_OP
#error "GLDC_N4_VERTEX_DMA and GLDC_S3_SEGMENTED_OP cannot be combined"
#endif

/* The existing N0/N1/N3 harness contains intentional direct-SQ baselines.
   Enabling global KOS vertex DMA changes pvr_list_begin ownership underneath
   those baselines, so a dedicated N4 harness must be used instead. */
#if defined(_arch_dreamcast) && GLDC_N4_VERTEX_DMA && \
    defined(GLDC_NATIVE_BENCH) && GLDC_NATIVE_BENCH
#error "GLDC_N4_VERTEX_DMA is incompatible with the N0/N1/N3 native benchmark"
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

/* Swap-time telemetry (2026-07-15/31 investigations): flush.c's
   wait/op/pt/tr/fin split + the sh4.c sprite-lane timers. Lives here (not
   flush.c) so BOTH files compile it out together — the sprite timers used to
   run unconditionally (Audit #001 OPA-12). 0 removes timer sampling;
   glKosTakeSwapTelemetry() still exists and returns an empty snapshot. The
   cheap event counters (grow/header/record) still tick. HyperSolar forwards:
   make all GLDC_SWAP_TELEMETRY=1 (or make gldc with the same switch). */
#ifndef GLDC_SWAP_TELEMETRY
#define GLDC_SWAP_TELEMETRY 0   /* returned to 0 (2026-08-06, quiet/ship profile) after the 2026-07-31 hunt */
#endif
