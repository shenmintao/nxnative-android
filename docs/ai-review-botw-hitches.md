# AI review: BotW gameplay hitches (response to the handoff)

Updated: 2026-11-02, Asia/Shanghai. Responds to [ai-handoff-botw-hitches.md](ai-handoff-botw-hitches.md).

This review is read-only: no device run, build, default change, release action or upstream merge. Numbers are re-derived offline from the retained captures under the handoff's `BENCH` root, mostly by `BENCH/review_hitch_offline.py`. Per-present op counts and replay costs are read directly from the runs' periodic log lines. The script uses only the standard library, imports no other bench helper, never calls adb and writes nothing. Upstreams were fetched for this review: nxnative is at `1ae435d`, four commits after `c646d43`, and none of the four touches prediction, prefetch or presentation. nx2android is unchanged at `8e030b4`.

Labels used below:

- **measured**: derived from retained timestamps or counters.
- **source**: follows from reading the code only.
- **inference**: consistent with the data but not isolated by it.

This review contains **no proven improvement**.

## 1. Conclusions first

1. **Every long replay stall reaches the display twice.** In all four captures, a long replay stall typically produces two consecutive >50 ms display intervals. The second interval is about as long as the first. It comes from the Vulkan present pacer, not from new host work (section 2, H1). Second gaps make up 43-49% of the >50 ms excess display time. In three of the four captures a second gap is also the longest interval. *(measured mechanism; fix not tested)*
2. **Each first gap is a serial cluster of pipeline compiles.** In the three timed captures, the >50 ms intervals coincide with graphics demand-miss records. The wide route joins presents to host frames exactly: 11 of 11 intervals. The other two runs are checked only against [previous present − 50 ms, present]: 14 of 14 in the original and 15 of 16 in the new run. For the large first gaps, the overlapping compile time is close to the gap length: 275.0 / 280.8 ms, 200.0 / 189.0 ms, 166.7 / 164.5 ms and 125.0 / 112.0 ms (gap / compile). Heavy records (≥15 ms) take 17-70 ms each, with a median of 36-37 ms. Inside a cluster they run back to back, 0.0-3.5 ms apart. Total demand-miss time on the replay thread is 946.0 ms per 120 s in the original build and 924.2 ms in the new one. *(measured)*
3. **The new prediction shortened no matched stall.** It admitted 9 jobs in the 120 s window and the replay waited on 7 of them. Each heavy wait lasted a full compile: 35.9, 48.5 and 34.5 ms. Route events that occur in both runs show the same compile time: 98.9 vs 105.3 ms, 120.2 vs 120.1 ms, 36.2 vs 35.9 ms and 37.5 vs 37.1 ms. The new run's lower maximum interval (200.0 vs 308.4 ms) has a different cause. At that route point the new run missed 4 + 3 pipelines split over two frames, where the original missed 8 in one frame. In both runs those compiles ran serially; prediction overlapped none of them. *(measured; small n)*

| Capture | >50 / >100 ms | Second gaps (of which >100 ms) | Second-gap share of >50 ms excess | Max interval |
|---|---:|---:|---:|---|
| `151653-user-live-hitch` (production 44) | 33 / 12 | 11 (6) | 1258 / 2742 ms = 46% | 241.7 ms, second gap |
| `154141-P2-wide-route-trace` (F416) | 11 / 4 | 4 (2) | 400 / 883 ms = 45% | 166.7 ms, first and second gap tie |
| `192400-original-pre-prefetch-once` (F416) | 14 / 6 | 5 (3) | 742 / 1508 ms = 49% | 308.4 ms, second gap |
| `182822-prefetch-moving-observation` (572c) | 16 / 5 | 6 (2) | 550 / 1283 ms = 43% | 200.0 ms, second gap |

## 2. Ranked hypotheses

### H1. Present pacing reserves the stalled frame's submission span *(measured mechanism)*

**Code path.**

- `pace_desired()` (`gfx_vk_present.c:777`) never schedules the next desired present earlier than `now + max(g, 4 ms) + 3 ms + 1.5 vsync`.
- For an ordinary frame, `g = vk_gpu_ms_recent()` (line 804). The buffered exception does not apply because the logs show "present snapshots: 0 copied".
- `vk_gpu_ms_recent()` (`gfx_vk_core.c:1296-1303`) returns the most recently finished frame's *sum of submission timestamp spans*.
- With split submission, the parts recorded before a compile are already on the GPU. The stalled frame's span therefore contains the stall: frame 11774 logs a 167.7 ms GPU span for a 171.7 ms host frame, and the original's frame 13530 logs 299.3 ms for a 299.6 ms host frame.
- The next frame is therefore scheduled one full stall later. The cadence keeps that latency until back-pressure blocks the game thread about seven frames later.

**For.**

- The wide-route attribution classifies five >50 ms intervals as `unknown`. Each one is the frame right after a `graphics_pipeline` frame. Their host costs are 13-22 ms, and they reach the display 66-186 ms after host work ends; the on-time median is 42.8 ms.
- The six pipeline-stall frames in that capture all match the pacer's reservation. In each case, the next frame's actual present minus (its host end + the logged GPU span + 15.5 ms) is +1.2 to +6.5 ms, which vsync snapping and the present thread's delay account for.
- Example sequence: frame 11774 occupies the host for 171.7 ms. Frame 11775 does 14.1 ms of host work and reaches the display 185.8 ms later. Frames 11775-11780 all stay at about 186 ms of latency. At frame 11781 the host blocks for 120.3 ms and latency falls to 85 ms.
- Game-side long frames are frames of ≥45 ms whose submit+present is under 40% of the whole frame. Inside the windows there are eight of them: 3 in the user capture, 1 in the wide route, 3 in the original and 1 in the new run. Every one comes seven frames after a 116-300 ms render stall. These are back-pressure, not new causes.
- No upstream fix exists to port. nx2android `8e030b4` has the same reservation and lacks even the local buffered-frame exception. nxnative's Metal path presents with `presentAfterMinimumDuration` (`nvn_mtl.m:11865`) and reserves no GPU span.

**Against / limits.**

- Exact present-to-host joins exist only for the wide route. For the other captures, the offset between log app time and monotonic time is calibrated from log ordering, to about ±0.1 s.
- The mean-fps effect is small. Second-gap excess is 0.33-1.05% of window time, which is about +0.2 to +0.6 fps. That is below the 1 fps gate even though it removes roughly half of the >100 ms intervals. Accepting a fix therefore needs a hitch metric agreed with the user.

**Smallest observation.**

- **Diagnostic, no build:** run the same route with the live `DM_VK_PACE=0`. The second gaps should disappear while the first gaps remain. Pacing off is already known to make intervals less even (perf-log, display-timing on/off/on), so use this only as a diagnostic.
- **Proof of a fix:** add an env-gated GPU estimate that ignores submission gaps. One option is the minimum of the last two finished frames; another is a cap near two periods, the same reasoning the code already gives for buffered frames. Test it with deterministic injected replay stalls in a fixed view (for example 10 × 150 ms) across ABAB phases. Measure the display excess within 1 s of each injection; it should fall from about 2S to about S.

### H2. Same-submission prediction can only add parallelism; it cannot hide compiles *(measured)*

`graphics_pipeline()` (`gfx_vk_exec.c:1042`) starts a scan only after a demand miss has been resolved (lines 1060-1062). The scan runs from `g_predict_next` (line 2321) to the end of the current submission. Replay takes 8.5-10.4 ms per frame for 2500-3700 draws. Any draw in the same submission can therefore be seen only a few milliseconds before it runs, while a heavy compile takes 17-70 ms (median 36-37 ms).

**For.** Changes in the periodic `window-end.log` counter snapshots account for all 7 waits:

| Snapshot change | What it shows |
|---|---|
| 1 wait, 6.438 ms | The record took 0.2 ms inline at the same event in the original run. The worker threads were created by that same kick. |
| 3 waits, 36.910 ms | One full 35.9 ms compile plus two sub-millisecond waits. |
| 1 wait, 0.489 ms | Cheap wait. One completed job in this group was never waited on; it falls among 0.1-0.5 ms records, so at most one compile may have been hidden. |
| 2 waits, 83.033 ms | Matches 48.5 + 34.5 ms, both full length. The 48.5 ms wait is longer than its inline neighbours: 40.4, 38.9 and 36.8 ms. |

**Upper bound.** Assume the best case for this approach: 3-way parallelism (replay plus 2 workers), zero lead time, perfect driver scaling, and use only on clusters with at least two heavy misses. That removes at most 218.7 ms of 884.4 ms heavy compile time in the new run, 393.0 of 914.9 ms in the original and 164.4 of 616.6 ms in the wide route. Clusters with a single heavy miss hold 54%, 30% and 46% of heavy compile time, which no same-submission scheme can reach.

**Against.** Only 9 jobs ran. Waits are assigned to records through snapshot changes, not individually.

**Smallest observation.** Log, for each job, the lead time from kick to demand and the worker build time, and compare them with inline build times. These are debug counters only, collected in one cold-start log run.

### H3. The scan rarely sees the next miss *(source; indirect evidence)*

The Android scan differs from upstream `pso_predict_rest_` (`nvn_mtl.m:7949`) in two ways.

**1. Where the scan stops.**

- Android returns at any op not on its list (`vk_pipeline_scan.h:83`). That includes `OP_UPDATE_UB`, clears, barriers, copies, dispatches, counters and events. It also returns at any change of render target (line 34).
- Upstream follows `OP_SET_RT` by recomputing attachment formats (line 7962) and replays only state for every other op (line 7989). It continues to the end of the submission, collects up to 160 candidates, then admits 2.
- In the new run, cumulative counters average about 3543 draws, 170 passes, 386 uniform updates (landed + superseded) and 21 dispatches per present. An Android scan therefore usually ends within a few draws. *(inference)*

**2. Which shaders must already be loaded.**

- `predict_program()` (`vk_pipeline_scan.h:6-15`) admits a draw only when `vk_shader_cached()` (`gfx_vk_shader.c:197`; trylock, no load) already holds every stage.
- A program that has not yet been bound in this run is therefore never admitted. Upstream first loads, in parallel, the shaders bound in the rest of the submission (`shader_prefetch_rest`, line 7908).
- The program-creation queue dropped 1579 requests at startup. The full-queue path does not mark a key as seen (`vk_shader_prefetch.h:37`), so a dropped program is retried only if the game sets it again or a scan passes it. The dropped count stayed at 1579 for the whole run.

**For.**

- There were 9 admissions against 21 heavy misses.
- At 146.5 s, two misses 0.9 ms apart (33.9 → 41.0 ms) were not admitted.
- At 231.8 s, the first three misses (3.5 and 0.7 ms apart) were not admitted. Only the fourth, 0.1 ms after the third, was. That fits it reusing the program the third had just bound. *(inference)*
- The 2-admission and 8-scan budgets are unlikely to be the binding limits at these counts.

**Against.** Nothing logs why a scan stopped, why a draw was rejected or whether a speculative key was ever used, so neither difference is measured. Guesses about integer views (um/im) and fragment-depth state (FDZ) could also produce unused keys.

**Smallest observation.** Add debug-only counters:

- a histogram of the op that stopped each scan;
- draw rejections by reason: shader not loaded, trylock busy, already built, duplicate, no free slot;
- for each demand miss, whether any scan had already seen its exact key.

One log-only run with recorded prewarm off produces hundreds of misses in minutes and needs no performance conditions.

### H4. Demand-first ordering and driver concurrency *(source; unknown)*

The first miss of each cluster always compiles before any speculative build starts. nxnative keeps this order on purpose: "Metal may serialize requests internally" (`nvn_mtl.m:6662`). Its large gains come from stand-ins that let a draw proceed without waiting (`hybrid_get`), not from lookahead. Whether the Adreno 840 runs concurrent `vkCreateGraphicsPipelines` calls in parallel is not measured. If it serializes them, for example on the shared `V.pcache`, then starting prediction before the demand build or adding workers would slow demand work.

**Smallest observation.** A standalone device microbenchmark: compile the 46-51 first-use keys at the tail of each run's `recorded-pipelines.bin`, with an empty pipeline cache, on 1, 2 and 3 threads.

### H5. Worker handoff and lock contention *(weak evidence; low priority)*

- The new workers run at nice 10 on CPUs 0-7 (`vk_compile_thread`, `gfx_vk_core.c:47`), while the replay thread is pinned to the prime cores.
- `vk_shader()` holds `g_sh_mu` through the bundle copy, the SPIR-V passes and `vkCreateShaderModule` (`gfx_vk_shader.c:222-316`). `bind_program()` takes the same lock on every bind (`gfx_vk_exec.c:939`). A descheduled low-priority holder can therefore delay the replay.
- The window saw only 83 shader fetches and 9 jobs. The observed costs are the two H2 cases: 6.4 ms against 0.2 ms, and 48.5 ms against about 37-40 ms.
- This is unlikely to drive the large stalls.

**Smallest observation.** Counters for replay-thread wait time on `g_sh_mu` and for worker versus inline build time.

### H6. Single misses need wider coverage or a path that does not wait *(measured bound)*

Single-miss clusters hold 30-54% of heavy compile time. The user's manual capture contains about 194 demand builds in about 120 s (presents 3600-10800). Its checkpoint counter rose by 388, but `pipes_built` counts each demand build twice (see the appendix). The scripted windows have 46-51 records each, so they sample first-use compiles roughly four times less often than real play. Prediction cannot remove these misses. Only two things can:

- broader recorded or derived keys;
- a path where the draw does not wait. The Vulkan counterpart of upstream's stand-ins would be something like GPL libraries built only on demand; the full-residency prototype failed at about 6 GiB.

## 3. Answers to the handoff's five questions

1. **Is prediction triggered early and often enough?** No, and starting it earlier inside the same submission would not change the outcome. The available lead time is a few milliseconds, against compiles of tens of milliseconds (H2). Only parallelism across a cluster can help, and that depends on coverage (H3) and on driver concurrency (H4).
2. **Are useful exact keys admitted?** Rarely: 9 admissions, and dense clusters went unseen. The likely limiters are the early stops and the requirement that shaders already be loaded (H3). This is not measured.
3. **Does speculative work delay demand work?** There are small measured cases: 6.4 ms against 0.2 ms, and 48.5 ms against about 37-40 ms. Lock-hold time is not measured. This is low priority at the current admission rate (H5).
4. **What explains the remaining stalls?** In the scripted captures every >50 ms interval is either a compile cluster (first gap) or a pacing echo (second gap). This is exact only for the wide route, where presents are joined to host frames; elsewhere it rests on the approximate alignment. The wide attribution's five `unknown` intervals of >50 ms are all frames the pacer delayed right after a stall frame; four of them are the second interval of a pair. The 25-50 ms intervals form a separate, smaller class: the wide attribution assigns 43% of intervals ≥25 ms to GPU completion waits and 22% to translation.
5. **How can a comparison control the workload?** Use per-event demand-miss time on route events that recur at the same time in each run (about 120.7, 129.2, 146.4 and 169.4 s here) rather than mean fps. Use same-APK startup toggles (`DM_VK_PIPELINE_PREDICT`, `DM_VK_SHADER_PREFETCH`) rather than two different builds. For pacing, inject deterministic stalls (H1).

## 4. Suggested order of next observations (each needs the user's approval for device time)

| # | Observation | Distinguishes | Cost |
|---|---|---|---|
| 1 | Live `DM_VK_PACE=0`, same route, 120 s | Whether H1 causes the second gaps | One run, no build |
| 2 | Env-gated GPU-estimate fix plus injected 150 ms replay stalls, fixed view, ABAB phases | Size of the H1 fix (about 2S → S) | Small debug build |
| 3 | Funnel and lead-time counters, cold-start log-only run | H2, H3, H5 | Small debug build; no performance conditions |
| 4 | Standalone compile benchmark on 1, 2 and 3 threads | H4 | Standalone tool |
| 5 | Same-APK prediction/prefetch 0/1, demand-miss time per recurring event | Net value of prediction | Route runs |

**Decisions for the user:**

- the acceptance metric for hitch work, since the mean-fps gate cannot register a halving of second gaps;
- whether to address pacing (H1) before more prediction work;
- whether prediction stays on by default, given that no benefit was measured.

## Appendix: method and caveats

- **Display intervals.** These are SurfaceFlinger `PresentFenceSignaled` times inside the `clock-bounds.csv` window. They reproduce each run's `result.csv` (for example, 7099 presents, 16 / 5 intervals, maximum 200.010937 ms). A "second gap" is a >50 ms interval that immediately follows another >50 ms interval. Excess is measured above 16.667 ms.
- **Demand-miss records.** Records come from `runtime-pipelines.csv` (MONOTONIC) and are converted with the offset in `monotonic-clock.csv`. "Heavy" means ≥15 ms. Records ≤5 ms apart form a cluster; records ≤30 ms apart form an event. The parallel bound packs each cluster's heavy records into three slots, longest first (LPT).
- **App-time offsets.** For longframe lines, the offset is the median of (end of the preceding record − longframe app time) over longframes >100 ms: wide 502109.487 s, original 515372.616 s, new 512099.751 s. A long frame counts as render-side when submit+present is ≥60% of the whole frame and as game-side when it is <40%.
- **`pipes_built`.** The counter increments once in `build_pipeline()` and once more in `graphics_pipeline()` for each demand build (`gfx_vk_exec.c` lines 1196 and 1044 at `e9f6c6e6`). Prewarm builds count once. For example, the new run's counter rose 1279 → 1293 for 7 misses.
- **Prediction counters.** These are cumulative snapshots. Assigning the 83.033 ms change to 48.5 + 34.5 ms is the only exact subset match among that group's records, but it remains an inference.
- **Scope.** No device run was made. Runtime, defaults, the release and saves are untouched. The review script is the only file added outside this document.
