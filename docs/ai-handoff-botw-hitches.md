# AI handoff: BotW pair warming, pacing and remaining first-use hitches

Updated: 2026-11-03, Asia/Shanghai. Repository: [shenmintao/nxnative-android](https://github.com/shenmintao/nxnative-android), workspace `D:\nxnative-android`. Work is on `perf/pair-warm`, from local main78975c37; supervisor reviews before merging. No merge, push or public release was authorized in this stage.

## 1. Problem and present conclusion

The user reports brief stutters after playing for a short time, sometimes seeing the displayed rate fall to 30-40 fps. This is broader gameplay than the short route used to finish performance plan 2. Recorded startup pipeline prewarm does not cover every subsequently encountered pipeline.

Graphics-pipeline creation explains part of the observed stalls. The earlier program-creation shader prefetch and upcoming-command prediction run and pass correctness checks, but have no established frame-time benefit. New program-level canonical pair warming is now shipped separately: its probe and one route pair show less heavy demand work, with a material memory cost and a differing-workload caveat. The whole hitch issue is not established as fixed; do not claim a general FPS gain.

The earlier prefetch comparison is one old-version run against an earlier new-version observation. The new observation is only 0.201 fps higher, inside the approximately 1% run-to-run noise floor. Its >50 ms interval count is higher, while its maximum interval and >100 ms count are lower. Actual periodic draw counts differ, so this is not a deterministic workload comparison. Prediction has no measured frame-time benefit and must not be presented as resolving first-use stalls.

A subsequent approved H1 diagnostic supports presentation-pacing amplification: with the same original F416 APK and only pacing disabled, second consecutive >50 ms gaps fall from5 to0 while substantial compile stalls remain. Ordinary late intervals worsen from1.30% to8.70%, so pacing-off is not retained. See the approved diagnostic below; the new prefetch mechanism still has no established performance benefit.

The retained median pacing estimate (`4a6b5c1e`, candidate2) shipped as06e382a5, now the immediate rollback. Fixed-view injected second gaps fall OFF12/12 -> ON0/12 with ordinary cadence/correctness passing. **Its pre-declared acceptance still fails criterion2:** ON/OFF excess0.609797>0.6, ON208.373140 ms versus allowed205.025319 ms, a3.347822 ms miss. That earlier user shipping override and route B zero second gaps/1.361021% late result remain valid historical records; pair warming changes neither pacing nor prediction semantics.

Current production is pair-warming release728b8a49, implementation4a50f628. **Its acceptance test did not pass: criterion4 RSS+168.734720 MB exceeds100 MB by68.734720 MB.** The user explicitly accepted that exact candidate as is, without a new candidate or changed threshold. Criteria1/2/3/5 meet their numerical limits; the route starts match but end views and draws diverge (4471 OFF/3601 ON), limiting attribution. Release smoke passes isolated save/reload plus30 s gameplay, PID18068 stable, RED0, default `on=1` and2089 warmed/failed0. Installed SHA, original preferences and all34 primary hashes match; app stopped/display OFF. First-use stalls, line topology, integer-view variants, the ~61 MB cache and memory remain open. New device work requires a separate bounded authorization and manual unlock; never handle credentials.

## 2. Exact scope and constraints

| Item | Setting |
|---|---|
| Game | The Legend of Zelda: Breath of the Wild, 1.9.0, update v1114112, title ID `01007EF00011E000` |
| Device | Lenovo TB323FU, SM8850P / Adreno 840, Android 16; adb serial `HA2PCY7Y` |
| System mode | Default balanced mode; no vendor/Game Mode/system performance changes |
| Rendering | Docked output 1920x1080, stock internal DRS 1280x720..1600x900; sampled at 1600x900 before/after both ordinary observations |
| Frame target | 60, `DM_FPS_HOLD=off` |
| Save | Fresh isolated copy of the verified owned Great Plateau seed; never play the primary save |
| Observation | 60 s stationary warmup, then 120 s moving capture of actual SurfaceFlinger presents |
| Route | Repeat `LUp~6.0`, `LDown~6.0`, `RRight~0.6`, with the existing acknowledgement/timing logic in `WideRouteBench` |
| Cache | Same verified driver-cache inputs and pipeline seed; startup reports 1209 prewarm keys, comprising graphics and compute keys |
| Quality | No lower resolution, precision, quality, skipped draws, or different frame target |

The route is an input schedule, not a recorded deterministic game replay. Wall-clock timing, weather/simulation phase, collision and camera progression are not locked. Start screenshots match location and view; periodic mean draw counts differ later. The wider diagnostic route can involve climbing and damage in the isolated copy.

CPU policy ceilings in both ordinary observations: policy0 1996800 kHz, policy6 2438400 kHz. Sampled CPU/GPU/skin thermal severity is zero before and after both runs. These observations do not establish performance on Snapdragon 8 Elite or Dimensity/Mali devices. Dimensity 9400 work is deferred.

## 3. Versions: distinguish source, debug observations and installed release

| Reference | Meaning |
|---|---|
| `b226609118ac575cbab10b1f5c6c0a8f62b58052` | Documentation snapshot before this handoff; records the requested original-version measurement |
| `da7947dffc0c1c72fb2af6447f1dad31e842f7c3` | Prefetch/prediction implementation and candidate packaging; tag `v0.3.0-rc2` |
| `e9f6c6e6` | Source parent of the implementation; useful for inspecting the change |
| `experiment/gpl-resident-20261102`, `16600918` | Local checkpoint of the rejected graphics-pipeline-library prototype; excluded from main and not pushed |
| `76f7a4c5`, `8ed6db5a` | Separate scheduling estimate/counters/tests; debug stall request read once per submission, not every draw |
| `4a6b5c1ea9a240d64b25cf24c637a053a5bd392f` | Candidate2: detect the same outlier, return median instead of cap; shipped by explicit user override |
| `2ee3d4ee` | Candidate2 stage2d evidence; criterion2 remains failed, before the later shipping decision |
| `78975c3738d4b27297c93f37f5511f331c8028bd` | Standalone pair-warming/parallelism probe record; merged into local main before this implementation |
| `4a50f62825be9770a35b2d4f2eb21495473c423d` | Program-creation canonical pair warming; exact normal/eligible FDZ modules, cache-only temporary PSOs |
| `a6c5d1fc7ef1d50a0b64a3d6bf2f84dfcad6f16d` | Original acceptance rejection and recovery; criterion4 remains failed despite the subsequent shipping override |

APK identities (full SHA-256):

```text
Old production: 44a337e0dbdb9dff087b70d0fe9a6cbd7bb76d5ff6c26fe8eccd5ce9af8ada33
Old debug:      f4164842fce02967fb2493a6d41568a7a4b3d2fc90cd67546f1992b78bdd321e
New observation:572c42add6f87bc5e14f5609a64b81ae8e10dc0add404c2e5791bffa1227040d
Rollback rc2:   bcd840c1a9a3427e6dbe4f46c5caa041d6caf5f9e0fb941243e2ca42ddcc988d
Candidate1 dbg: 17df9345dbc9b0c9fc9aa836338bf083d6f7afe083c3c19688621a1a2e9d02b3
Candidate2 dbg: d4d24c6caa9b09373cf6d8e3e182e3752896b825193336820ad1e23d13398792
Reused V debug: e6013986cdd98bbb47c5c85f14f11d97027068a43e86aa70ded45484497fd39d
Rollback pacing:06e382a5ecab44af72236bba948f14544c74a2e8089f8ec9fc44f293689b041c
Pair warm dbg:  c00581ddcec48fb02fdafe6f04e49a4e5677cf3c66b4901840de8e4ec2cd30b8
Pair warm V:    cc9cdf1f98e3f1fb19e61a29d2e5b9c9f95f63ff72da6a493c5f73ca9dda939a
Current release:728b8a491cce5f524bf673dadc45f666a8e7a41d9366dbe6913bd67d6a9c5568
```

"Original" in the prefetch comparison means the retained implementation before those two prefetch changes, not fresh upstream defaults. That new observation predates two final review guards. Pacing release3 evidence comes from debug3; pair-warming OFF/ON uses the same c00581dd debug APK with one startup switch changed. Each release received isolated save/load and30 s correctness smoke, not an extra release performance comparison. App version remains1.9.0/code1114113; no version bump or release notes.

Current production is `BENCH/pair-warm-release.apk`, also exported to `ROOM/ZeldaBreathOfTheWild.apk`. Immediate rollback is `BENCH/pace-budget-release3.apk` / `ROOM/ZeldaBreathOfTheWild-pace-budget-06e382a5.apk` (06e382a5); older rollback is `BENCH/prefetch-rc2-release.apk` / `ROOM/ZeldaBreathOfTheWild-prefetch-bcd840c1.apk` (bcd840c1). Older44 backups/data remain intact. `BENCH/pair-warm-promotion.json`, `pair-warm-promotion-verification.json` and group20261103-093205 hold installed/exported/rollback hashes, preference readbacks and34-file save archives. Data/modules and primary saves are unchanged; app stopped/display OFF, no input/keeper loop remains.

Future shared-harness restores use hash-verified `BENCH/current-production.json` through `production_runtime.py`, both at construction and restoration. It overrides stale06e382a5/bcd840c1 arguments with728b8a49; an offline mock exercises the real restore method without adb. Historical custom one-shots/snapshots can embed their own superseded targets and still require inspection and new authorization; never run/import them blindly.

The earlier GitHub rc2 release is a **draft candidate** with the generic Windows tool only. Game APKs/files/saves/keys stay private. These promotions change only local private production and records; no GitHub release, push or merge occurred.

## 4. Measurements and what they do not prove

### Pair-warming probe, route acceptance failure and user shipping override

The standalone shell probe extracts the runtime SPIR-V transformations/layouts/full pipeline builder and enabled features. Calibration on22 first real heavy-pair keys is19.602 ms versus in-game35.502 ms, within the approved0.5-2x interval; shell clocks exceed game clocks. Across62 pairs/251 real keys, canonical warming yields241 cheap<=2 ms, one partial and nine>10 ms; all11 cache misses change FDZ. Ordinary controlled state changes hit the application cache, but triangle-to-line topology misses62/62 (median9.498 ms). Cross-thread warming hits62/62 (median0.114 ms); saved VkPipelineCache reload hits62/62 (0.092 ms) versus9.022 ms with an empty cache. Six nice10 workers on0-7 scale4.35x; prime6-7 peak at two workers. Probe sources/raw CSVs are private under BENCH/pair-warm-probe; this does not measure game FPS or exhaustively warm15,767 static pairs.

| Single120 s route metric | OFF080257 | ON080717 | Pacing C2-B002509 context |
|---|---:|---:|---:|
| Heavy graphics demand>=15 ms records / ms | 23 /913.960 | 3 /87.182 | 15 /576.039 |
| All graphics demand records / ms | 50 /953.689 | 40 /112.241 | 39 /607.014 |
| Average fps | 59.212937 | 59.801313 | 59.371859 |
| >17.5 ms intervals | 1.407063% | 0.724436% | 1.361021% |
| p99 / maximum, ms | 25.001355 /216.678646 | 16.667761 /58.336614 | 25.001354 /166.675886 |
| >50 / >100 / consecutive second gaps | 10 /4 /1 | 1 /0 /0 | 6 /3 /0 |
| Mean draws | 4470.75 | 3601.33 | 4503.33 |
| End RSS / TOTAL PSS, KiB | 2418604 /3417402 | 2583384 /3583070 | not sampled here |

OFF/ON use fresh isolated owned seeds, original driver-cache inputs and1209 keys,60 s fixed warmup, the same six-second input route,1080p output/stock1600x900 main pass, balanced1996800/2438400 kHz caps and sampled severity0. Start views align; end views diverge (OFF wall, ON looking down near water/grass), mean draws differ19.45%. The input schedule is not deterministic replay. Numerical reductions are single-run observations with workload confounding, not an established general FPS gain. No repeat or threshold change followed rejection.

Criteria1/2/3/5 meet their numerical limits:3/23 heavy=13.04%<=30%; late0.724%<=1.907%; first launch->game52.469 s versus OFF52.610 s (allow+10 s); later52.344 s within2 s. V075846 passes normal rendering with0 errors and exactly the three text-matched182026 unused-output warnings; debug RED0/PIDs stable. **Criterion4 FAILS:** RSS+168734720 bytes=168.734720 MB/160.918 MiB, above100 MB (and100 MiB), miss68.734720 MB; PSS+169.644032 MB. User explicitly ships this same4a50f628 candidate despite that result. `acceptance_passed=false` and failed criterion4 are preserved in acceptance/promotion JSONs; shipping is an override, not a retrospective pass.

ON counters1967 queued/warmed,5419 skipped,0 failed,230220.496 cumulative worker ms,high1024/pending0. Later081135 reloads61003544 bytes (~61 MB decimal) from ON, keeps learned keys and resets only the isolated seed; zero graphics demand from launch through30 s gameplay. Later3318 admissions/0 failures are consistent with faster cached work; skip reasons and subsequent growth are not isolated. Original-cache OFF is23 heavy/913.960 ms versus C2-B15/576.039 ms, so historical compile work also varies.

Release smoke093216 uses the unchanged728b8a49 APK, isolated manual save/reload of the new09:37 Plateau slot and30 s gameplay; PID18068 stable, RED0,1209 keys and visible default `on=1`,2089 warmed/failed0. The env omits `DM_VK_PAIR_WARM`; release ignores DM_CTL. Only isolated save/2 data changes, all34 primary hashes/preferences match and app stops/display turns OFF. Attempt091102 saved but hit the controller15-minute input deadline before reload; it restored06e382a5 and is not a pass. The same APK succeeds with100 ms game-only keys. Both attempts and recovery proofs remain archived.

Open items are line/point topology, integer-view (`um/im`) and vertex-input retyping variants not warmed by the canonical key; remaining first compiles; ~61 MB cache persistence/growth; extra memory attribution and longer-session/static-set residency; and queue-pressure/lead-time coverage. No full static bootstrap or general-gameplay soak has been measured. The old one-canonical-state probe cache/time projections are not normal+FDZ application measurements. Further diagnosis needs a separately approved bounded protocol; prediction still has no measured benefit.

### Pacing candidates and explicit shipping override

Each A run uses one process,60 s fixed-view warmup, then OFF/ON/ON/OFF60 s phases with six150 ms injected replay stalls each,>=2 s/log-confirmed settling, actual SurfaceFlinger presents and monotonic clock mapping. The injection occurs at a draw after a split part has reached the GPU; present N joins the delayed GPU span of frame N-1. Candidate1 GPU spans175.0-191.8 ms, candidate2 172.3-197.4 ms, so all24 in each run reproduce the elapsed submission gap. No timed natural demand record overlaps these injection windows.

| Metric / unchanged criterion | Candidate1 debug2 | Candidate2 debug3 |
|---|---:|---:|
| A run | `20261102-234223-pace-budget-A` | `20261103-001808-pace-budget-C2-A` |
| OFF / ON second gaps | 12/12 /6/12 | 12/12 /0/12 |
| OFF / ON median excess,16.667 ms base | 345.875489 /216.706390 ms | 341.708864 /208.373140 ms |
| ON/OFF ratio required<=0.6 | 0.626545: fail | 0.609797: fail by3.347822 ms |
| Criteria0/1/2/3/4 | pass/fail/fail/pass/pass | pass/pass/fail/pass/pass |
| Clean late%, OFF1/ON1/ON2/OFF2 | 0.670241/0.905736/0.670241/0.770003 | 0.602611/0.636090/0.737513/0.535475 |
| Clean p99 | 16.667709-16.667762 ms | 16.667709-16.667812 ms |
| B run | `20261102-234924-pace-budget-B` | `20261103-002509-pace-budget-C2-B` |
| B fps / late>17.5 ms | 59.157387 /1.506830% | 59.371859 /1.361021% |
| B >50/>100 / maximum | 11/3 /266.681406 ms | 6/3 /166.675886 ms |
| B second gaps / excess | 1 /50.003801 ms | 0 /0 ms |
| B total>50 ms excess | 841.723179 ms | 466.697867 ms |

Candidate2 phase excess medians OFF1/ON1/ON2/OFF2=345.875739/216.706338/204.206265/341.708864 ms; both ON phases are below both OFF phases, but the combined ratio still fails. The user reviewed this exact miss and approved shipping; do not relabel A or overall pre-declared acceptance as passed. B passes second gaps<=1 and late<=1.8%; historical182822 has six second gaps/1.112990% late,192400 five/1.300537%,211447 zero/8.695043%. B is one historical comparison with different draw/compile work and does not prove a general FPS improvement.

V `20261102-233104-pace-budget-V` is reused without rerun because candidate2 changes arithmetic only; Vulkan calls/usage are identical. Errors0, exactly three unused-output warnings match182026 byte-for-byte (text SHA `ef0312909c3fc12e23c48e84f6c5f371d3168d9d6094defcf699b5ea72a30570`, numeric message ID/VUID was not logged), no other RED. Both injections and budget states work. A/B require and have RED0, stable PIDs. A/B use balanced caps1996800/2438400 kHz,1080p output, sampled internal1600x900,1209 keys and all64 foreground thermal samples severity0. Screen-on cooling with three consecutive severity0 observations replaces display-off cooling so manual unlock remains valid.

Supplementary, non-acceptance requested-period metrics are bounded estimates from read-only log polling only during candidate2 injection windows; legacy candidate1 durations are unavailable. Candidate2 median requested-period excess OFF325.045/ON170.880 ms and interval-2 duration OFF422.077/ON235.008 ms have coarse bounds (duration-bound widths309-746 ms). They do not change acceptance. Game1->2 requests follow all24 candidate1 stalls, but host held1/2/3 summaries are1800/0/0; an effective pacing period33.3 ms and exclusive attribution of later25-33 ms intervals to it are not established.

Release3 smoke `20261103-004043-coverage` passes30 s gameplay, manual save/reload of the new00:44 Great Plateau slot, PID4685 stable and RED0. Thirty-eight600-frame display lines confirm `budget_on=1`; the final checkpoint reports seven trims/236.506 ms, cumulative over title/game/menu/load rather than a performance window. Only the isolated save changes; primary/preferences verification and production promotion are recorded separately from the failed numerical acceptance.

### Requested single original baseline versus the earlier new observation

| Metric | Original debug F416 | New debug observation 572c |
|---|---:|---:|
| Run ID | `20261102-192400-original-pre-prefetch-once` | `20261102-182822-prefetch-moving-observation` |
| Capture duration | 120.048554 s | 120.047094 s |
| Actual presents | 7075 | 7099 |
| Average fps | 58.934487 | 59.135126 |
| Intervals >17.5 ms | 1.300537% | 1.112990% |
| p99 interval | 25.001354 ms | 25.001354 ms |
| Intervals >50 ms | 14 | 16 |
| Intervals >100 ms | 6 | 5 |
| Maximum interval | 308.350416 ms | 200.010937 ms |
| Mean draws in periodic samples | 4450.417 | 3706.750 |
| Audio callback increase | 24000 | 24000 |
| RED diagnostics / validation errors | 0 / 0 | 0 / 0 |
| Presentation-latency crosschecks | 127 | 127 |

New minus original: +0.200639 fps (+0.34%), -0.187548 percentage points of late intervals, two more intervals >50 ms, one fewer >100 ms. The p99 is unchanged. A lower single maximum is not proof of a stable reduction in stutter. The draw-count difference is a workload-comparability limitation, not a normalization factor or evidence that draws were skipped.

The user waived OFF/ON/ON/OFF, then requested only this one old-version measurement. There are no repeated paired windows. The local normal acceptance rule (at least 1 fps, both ON windows beating both OFF windows) has not been met. A previously agreed alternative late-frame threshold is also not established by this pair. Do not quietly restart the full comparison.

### Earlier diagnostic evidence: separate workloads and overhead

- Manual user-play capture `20261102-151653-user-live-hitch`: 120.112 s, 58.004 fps, 33 intervals >50 ms, 12 >100 ms, maximum 241.680 ms. The periodic pipeline counter grows from 1301 to 1689 after prewarm. The old demand path increments that counter twice per graphics build, so +388 means approximately 194 demand builds, not 388 distinct creations. This is a different user-controlled route, not a matched old baseline for the new observation.
- Expanded diagnostic `20261102-154141-P2-wide-route-trace`: 59.186944 fps, 11 intervals >50 ms, four >100 ms, maximum 166.675938 ms. Timed attribution maps 7104/7104 presentation intervals to host frames and classifies graphics creation as primary in 6/11 long intervals. It records 42 runtime graphics-creation calls, zero compute-creation calls, 635.342 ms graphics-creation wall time in total, and a largest individual interval of 64.077 ms. Diagnostic attribution is evidence of a partial contributor, not a counterfactual proof or complete explanation.
- Old plan 2 short-route results (fixed 59.895 fps, moving 59.830 fps, no >50 ms intervals) cover a different, simpler route. Do not use those numbers to declare this broader user-play problem resolved.

### Subsequent approved H1 diagnostic: same APK, pacing off

Run `20261102-211447-H1-pace-off-once` repeats the original192400 F416 setup with only `DM_VK_PACE=0` changed. Second consecutive >50 ms gaps fall5 ->0, their excess above16.667 ms falls741.712292 ->0 ms, >100 ms intervals6 ->3, maximum308.350416 ->158.342031 ms. Six demand clusters >=50 ms remain (47 graphics-demand records /838.435050 ms, versus51 /945.985879 ms). Total >50 ms excess falls1508.429376 ->733.382913 ms. This supports H1's repeated-reservation mechanism, with the limits of one historical pair and non-identical compile work.

Ordinary cadence worsens: >17.5 ms intervals1.300537% ->8.695043%, while mean fps58.934487 ->59.498442 and p99 stays approximately25.001 ms. Pacing-off is diagnostic only. Resolution, caps, thermal levels and prewarm match; periodic mean draws4450.417 ->4449.167. Then-current production bcd840c1 and the original preferences/primary saves were verified/restored; that diagnostic made no runtime/default change. The subsequent pacing-preserving estimate is measured above; pacing remains enabled in current production.

#### Mechanism and scope of a prospective fix

There are two costs to distinguish. First, a newly encountered pipeline can block replay while it is created. Second, presentation scheduling can add another delay after that work has ended. With split submission, the GPU may execute early commands and then wait for the CPU to produce the rest. The first/last GPU timestamp span can include that idle gap; it is elapsed time, not a measurement of uninterrupted GPU execution.

`vk_gpu_ms_recent()` exposes the recent submission span. The original `pace_desired()` used it directly as a budget for the next requested presentation, with margins and vsync snapping. A span inflated by an earlier compile stall could therefore push the cadence out again. In the retained original run, one275.015 ms display interval is followed by308.350 ms. Disabling the pacer removes the five consecutive second gaps while six substantial compile-demand clusters remain, supporting this amplification mechanism. It does not prove that every gameplay hitch has the same cause.

Current production implements that separation: `vk_gpu_ms_budget()` gathers up to eight recent known finished spans in the same ring order as the raw statistic, and the pure `vk_pace_budget_estimate()` detects `latest > max(2*median, period_ms)` with>=3 known samples, returning median for an outlier and latest otherwise. Candidate1 returned the cap; candidate2 changes only the replacement. `pace_desired()` uses this estimate for non-buffered frames with `DM_VK_PACE_BUDGET` default ON (debug live re-read every30 frames). Raw `vk_gpu_ms_recent()` and its timing/ADPF callers, the game cadence,4 ms floor, GPU completion synchronization and presentation feedback are unchanged. Existing600-frame logs append budget state and cumulative trimmed count/ms. A debug-only one-shot150 ms replay-stall request can reproduce the mechanism; it is absent from release. Genuinely changing GPU workloads remain a robustness limitation, and first pipeline-creation stalls are still a separate open problem.

## 5. What is implemented, and known limitations

Source was adapted from nxnative `c646d43d567fbd1b96777ff7f3bd6d9aabb1a36a` (`mtl_prog_prefetch`, pending builds, `pso_predict_rest`). The reviewed nx2android reference was `8e030b41`, already an ancestor of this branch. These were targeted upstream reviews, not a complete audit of all Metal features. A future fix should fetch and search the relevant upstream functions again before inventing a replacement.

1. **Program-creation shader prefetch.** The existing `g_nvn_on_shader` callback queues known bundle keys. One low-priority worker owns a bounded 256-entry queue; jobs copy keys/stages and keep no guest program pointer. Seen keys are deduplicated. A full queue leaves ordinary demand loading intact. Unsupported stages stay on the demand path. Default on; `DM_VK_SHADER_PREFETCH=0` disables it at startup.
2. **Upcoming-command pipeline prediction.** A private replay-state shadow walks upcoming records with a bounded 8192-record guard, nested call/jump handling, two admitted builds and eight scans per logical submission, at most two outstanding jobs. Changed targets and memory/compute/synchronization operations stop speculation. Only cached shaders are inspected without blocking. Uncertain view/FDZ variants can produce an unused speculative key; the actual draw computes its own exact key.
3. **Exact demand fallback.** A draw uses a completed exact pipeline, steals a queued job and compiles normally, or waits for its exact running job. The new mechanism does not skip draws. The pre-existing skip-draw asynchronous option remains disabled by default. Ordinary full Vulkan pipelines are used; the 1000-byte PPE2 key ABI is unchanged. `DM_VK_PIPELINE_PREDICT=0` disables prediction at startup.
4. **Worker scheduling.** New workers use nice10 and restore the app leader's allowed CPU mask. The measured mask is 0-7; foreground replay affinity remains prime CPUs6-7. This is not a reserved or exclusive background core, and does not prove absence of CPU/cache contention.
5. **Pacing scheduling estimate.** The median replacement remains default ON, inherited unchanged from release3; it does not alter raw timing or prediction. Historical pacing tests6/6, source162=159 passed/3 optional Yaz0 skips, debug3/release3 each compile144 sources. Its separate A criterion2 failure and user override remain recorded.
6. **Program-level pair warming.** Optional `g_nvn_on_program` owns only known keys from SetShaders and resolves stages from the bundle. Queue1024/dedup8192, up to four lazy nice10/app-mask workers, held until all startup-prewarm workers exit. Missing/partial/compute/tessellation/xfb/unsupported-GS/interface cases remain demand work. Canonical triangle keys derive matching SPIR-V numeric types with texture masks/vin zero; ordinary builder/layouts create normal and eligible FDZ pipelines, immediately destroyed without runtime-map/PPE2 insertion. Only V.pcache warmth remains; existing approximately30 s persistence applies. `DM_VK_PAIR_WARM` startup default ON, release logs cumulative counters and ignores DM_CTL. This adapts grouping/order concepts from upstream65501fbb; Metal pulling/archive code is not ported.
7. **Current checks and bounds.** Pair/native/release source suite165=162 passed/3 optional `DM_YAZ0_LIB` skips; debug/release144 sources compile, validation V passes0 errors/3 baseline warnings. Release source/library hashes and runtime counters prove default ON. Memory criterion4 remains failed; the user approved shipment despite it. Destroying PSOs does not establish a bounded total module/driver-cache RSS or eliminate variant/topology misses.

In the new observation's final periodic snapshot: 3729 shader fetches queued/completed, 1579 queue-pressure deferrals; pipeline prediction nine queued, eight completed, one stolen, seven waits / 126.870 ms. These are cumulative snapshots, not counters isolated exactly to the 120 s window. Deferral attempts can repeat keys. Do not equate them to unique missing shaders, cache hits or eliminated compiles.

Historical prefetch correctness evidence: 156 source tests, all 144 Android sources build; cold first-use Vulkan validation `20261102-182026-coverage` with recorded prewarm disabled has zero errors and three unused-output warnings. It exercises 3264 completed shader fetches and 44 predicted jobs (42 complete, two stolen); all three new worker masks are0-7. Validation overhead selects stock1280x720, so that run is not performance evidence. The final small guards passed 12 affected checks and the rc2 release smoke. Current pacing/release evidence is listed above. The final30-minute soak remains waived; full-game compatibility is unverified. An existing lower-leg ground-occlusion issue remains.

## 6. Code to review first

| File / function | Relevance |
|---|---|
| [gfx_vk_exec.c](../nxnative/dm/android/gfx_vk_exec.c): `graphics_pipeline`, `pipeline_key`, `build_pipeline`, `pipeline_observe_*`, `vk_exec_reset` | Demand path, exact key, prediction trigger, budgets and measurement scope |
| [vk_pipeline_scan.h](../nxnative/dm/android/vk_pipeline_scan.h): `pipeline_predict_rest`, `predict_program` | Lookahead boundaries, shader availability, admission coverage and speculative-key accuracy |
| [vk_pipeline_jobs.h](../nxnative/dm/android/vk_pipeline_jobs.h): `pipeline_predict_kick/take`, worker, publication | Queue admission, stealing, exact waits, deduplication and cache publication |
| [gfx_vk_shader.c](../nxnative/dm/android/gfx_vk_shader.c): program callback, `vk_shader_cached`, `vk_shader`, shader variants | Program creation timing, module loading and global shader-mutex contention |
| [vk_shader_prefetch.h](../nxnative/dm/android/vk_shader_prefetch.h) | Queue pressure, deduplication and ownership |
| [gfx_vk_core.c](../nxnative/dm/android/gfx_vk_core.c): `vk_compile_thread` | Priority and inherited/app-leader affinity handling |
| [tests/test_vk_prefetch.py](../tests/test_vk_prefetch.py), [tests/native/vk_prefetch.c](../tests/native/vk_prefetch.c) | Actual production-header tests: queue bounds/ownership, exact waits, generation, replay state and boundaries |
| [vk_pace_budget.h](../nxnative/dm/android/vk_pace_budget.h), `gfx_vk_core.c:vk_gpu_ms_budget`, `gfx_vk_present.c:pace_desired` | Separate robust scheduling estimate, original raw timing and cadence preserved |
| [tests/test_vk_pace_budget.py](../tests/test_vk_pace_budget.py), [tests/native/vk_pace_budget.c](../tests/native/vk_pace_budget.c) | Median/outlier/period arithmetic, ring/pacer behavior, debug release-exclusion checks |
| [hle_nvn.c](../nxnative/dm/gfx/nvn/hle_nvn.c): `nvnProgramSetShaders`, `g_nvn_on_program` | Optional callback supplies copied known keys; other backends default to no callback |
| [vk_pair_warm.h](../nxnative/dm/android/vk_pair_warm.h), [vk_pair_queue.h](../nxnative/dm/android/vk_pair_queue.h) | Canonical interface/state, exact normal/FDZ builds, startup gate, queue/dedup/ownership/counters; no retained PSO or recorded key |
| [tests/test_vk_pair_warm.py](../tests/test_vk_pair_warm.py), [tests/native/vk_pair_warm.c](../tests/native/vk_pair_warm.c) | Bounded admission/dedup/retry/wrap, eligibility/interfaces, startup gating, FDZ destroy and default/off/DM_RELEASE checks |

Read-only implementation diff:

```powershell
git diff e9f6c6e6 da7947df -- nxnative/dm/android/
git show 16600918 --stat
```

## 7. Specific review questions, not established causes

1. **Is prediction triggered early/often enough?** `graphics_pipeline` returns on the last-key fast path and on an existing pipeline-cache hit. Its call to `pipeline_predict_rest` comes after a demand miss is resolved. Check whether long sequences of cached draws fail to initiate useful ahead-of-demand work, or the first cold draw already blocks before prediction starts. This is a code-path observation; the missing opportunity and its cost have not been measured.
2. **Are useful exact keys admitted?** Check cached-shader availability, state-dependent variants, stopping boundaries, budget exhaustion and lead time. There is no measured admission funnel or exact speculative-hit coverage. A queued/completed count alone does not show useful overlap.
3. **Can speculative work delay demand work?** `g_sh_mu` protects shader loads/variants; pipeline-cache/layout locks and shared CPU/cache resources may serialize or compete. Inspect actual lock scope and scheduling before proposing more workers or affinity changes. No measured lock-hold attribution establishes this as a regression.
4. **What explains remaining stalls?** The earlier trace attributes only some long intervals to pipeline creation. Resource streaming/decompression, upload work, game-thread synchronization and GPU dependency waits remain questions, not diagnosed answers. Zero new compute-pipeline creations does not mean zero GPU compute work. Align each selected long present interval to its serial critical path before changing another subsystem.
5. **How can a smaller comparison control the workload better?** Same stick timings do not ensure the same scene/draw mix. Propose a bounded, user-reviewed method if new testing is necessary; do not claim an improvement from the current pair or automatically schedule repeated runs.

Please return ranked hypotheses with file/function references, the evidence for/against each, and the smallest observation that would distinguish them. Distinguish a source-level opportunity, a measured bottleneck and a proven improvement.

## 8. Rejected or incomplete approaches

- Recorded prewarm helps previously encountered pipelines, but cannot guarantee every unvisited route. Scanning resources found 15,767 known shader pairs; a shader pair is not a complete pipeline key and does not exhaust runtime-generated shaders or fixed-function/attachment/view variants.
- Vulkan graphics-pipeline libraries ("GPL" here) were tested on the Adreno840, which advertises library support and fast linking. A zero SPIR-V generator-version word triggered real-library driver rejection; a header-only nonzero low-version workaround allowed small validation to pass without changing shader instructions.
- Full residency was not feasible in that prototype: after >15,000 pairs / >62,000 libraries, the app aborted at roughly587 s; last RSS6296796 KiB (~6.0 GiB). Android reports EXIT_SELF/status134, not LOW_MEMORY. High memory is observed; the abort's root cause is not established. No performance comparison ran. The prototype is quarantined at local16600918 and must not be silently merged or described as enabled in rc2.
- The upstream lock-poll rule, ADPF changes, memory lookup changes, real gyro/accelerometer input, rumble and `DM_HITCH_DUMP` are already present. Extra sync-caller pinning is disabled. Do not re-list these as unported mechanisms or repeat the completed lock experiments without new evidence.

## 9. Raw evidence and reproducibility index

Private local artifact root, abbreviated `BENCH` below:

```text
C:\Users\shenmintao\Downloads\KUANYE.part1\publish\ROOM\01007EF00011E000_build\android\bench
```

| Under BENCH | Contents / purpose |
|---|---|
| `original-once-comparison.json` | Both CSV rows, matching-condition checks, deltas and comparison caveats |
| `runs/20261102-192400-original-pre-prefetch-once/` | Requested original run |
| `runs/20261102-211447-H1-pace-off-once/`, `H1-pace-off-comparison.json` | Approved pacing-off diagnostic, compared with the same original F416 APK |
| `runs/20261102-182822-prefetch-moving-observation/` | Earlier new-build observation |
| `runs/20261102-154141-P2-wide-route-trace/` | Earlier expanded-route diagnostic with component tracing |
| `runs/20261102-151653-user-live-hitch/` | User's original manual-play diagnostic |
| `runs/20261102-182026-coverage/` | Cold first-use Vulkan correctness validation |
| `runs/20261102-190024-coverage/` | Historical rc2 bcd840c1 isolated save/load and30 s smoke |
| `pace-budget-build.json`, `pace-budget-validation.json`, `pace-budget-validation-stage2d.json` | Immutable APK/source identities, both candidates, per-injection/cadence tables, unchanged failed criterion2 and later explicit shipping decision |
| `runs/20261102-233104-pace-budget-V/` | Reused correctness validation with three exact baseline warnings, no errors/regressions |
| `runs/20261103-001808-pace-budget-C2-A/`, `runs/20261103-002509-pace-budget-C2-B/` | Candidate2 full A and single B evidence; candidate1 paths remain indexed in JSON |
| `runs/20261103-004043-coverage/`, `groups/20261103-004026/` | Release3 smoke, inspected save/load screenshots, isolated-save change and34-file primary/preferences recovery audit |
| `pace-budget-promotion.json`, `pace-budget-release-smoke.json`, `pace-budget-promotion-verification.json` | Historical pacing shipping override, then-installed/rollback identities, installed hash and offline verification |
| `pair-warm-probe.json`, `pair-warm-probe/` | Standalone calibration,62-pair real/control/state/thread/persistence/scaling results, extracted source/build/raw CSVs; shell-only probe files were removed from the device |
| `pair-warm-build.json`, `pair-warm-validation.json`, `pair-warm-user-override.json` | Unchanged APK/source identities, all five criteria, route/memory/counter tables, draw-divergence caveat and explicit failed-RSS shipping override |
| `pair-warm-verification.json`, `*-before-shipping-override.json`, `pair-warm-harness-snapshot/` | Raw-arithmetic/source/APK audit distinguishes validation-stage06e382a5 recovery from later728b8a49 production; before-override archives/snapshot preserve original acceptance/rejection evidence |
| `runs/20261103-075846-pair-warm-V/`, `runs/20261103-080257-pair-warm-OFF/`, `runs/20261103-080717-pair-warm-ON/`, `runs/20261103-081135-pair-warm-LATER/` | Correctness, original-cache OFF/ON and ON-produced-cache relaunch, matching-condition/memory/thermal/present artifacts |
| `runs/20261103-091102-coverage/`, `groups/20261103-091045/`, `pair-warm-release-smoke-attempt1.json` | Incomplete first release smoke: save completed, input deadline before reload; production/preferences/34 saves restored; not counted as passed |
| `runs/20261103-093216-coverage/`, `groups/20261103-093205/` | Successful unchanged-release save/reload/30 s smoke,100 ms game-only key inputs, release default counters and installed/exported/preferences/34-save audit |
| `pair-warm-promotion.json`, `pair-warm-release-smoke.json`, `pair-warm-promotion-verification.json` | Current728b8a49 production, both rollback identities, criterion4 still failed/user override, independent real-restore-method mock without adb |
| `pair_warm_release_smoke.py`, `verify_pair_warm_promotion.py` | Approved override controller and offline promotion audit; controller requires fresh authorization/unlock, not casual import/run |
| `current-production.json`, `production_runtime.py` | Current hash-verified728b8a49 restoration selection used by shared Bench constructor/restore; immediate06e382a5 and older bcd840c1 rollback identities |
| `pace_budget_release_smoke.py`, `verify_pace_budget_promotion.py`, `pace-budget-stage3-harness-snapshot/` | Stage3 controller, offline audit and exact execution-source snapshot before restoration helpers were updated |
| `wide-route-attribution.log`, `attribute_late_frames.py` | Attribution invocation/results index and analysis implementation |
| `compare_original_once.py`, `summarize_original_once.py` | The latest single original run and offline summary; the first script controls the device, do not import/run casually |
| `pace_off_once.py`, `summarize_pace_off.py` | Approved H1 diagnostic and offline paired-gap summary; the first controls the device |
| `prefetch_smoke.py`, `prefetch_common.py`, `diagnose_route_hitches.py`, `bench.py`, `present_trace.py` | Previous new observation, scene checks, route, isolation/restoration and actual-presentation extraction |
| `groups/<timestamp>/` | Original env/settings, primary-save hash archives, restore APK identity and restoration audit |

For each ordinary run, begin with `result.csv`, `env.txt`, `before.png`, `after.png`, frequency/thermal files, `window-start.log`, `window-end.log`, `runtime-pipelines.csv`, `present-fences.csv` and `clock-bounds.csv`; pair warming also records raw VmRSS/dumpsys PSS and startup clocks. Raw capture is `frames.perfetto-trace`; extracted SQL/CSV and latency crosschecks remain alongside it. Scripts are private analysis helpers, not public bundle contents. Historical helpers can run at import or embed old restore targets; inspect first. The current shared selector resolves728b8a49 even for stale arguments; archived snapshots are historical evidence, not launchers.

**Counter caveats:** the new graphics timing records cover demand-miss intervals, including exact predicted-job waits; background creation is recorded separately. The collector's CSV label calling these "exact runtime creation calls" is too broad for the new renderer. The new46 and original51 timed graphics records cannot be treated as driver-compilation counts or cache-hit rates. The old periodic `pipes_built` counter double-counts graphics demand builds; prewarm builds increment once, so dividing arbitrary mixed counters by two is also incorrect. Generic Android `mtl_*` pipeline/upload counters are fixed-zero stubs; zero values do not exclude actual work. GPU timestamp envelopes may include submission gaps and must not be summed with overlapping CPU waits as independent frame costs.

## 10. Related records and current boundary

- [Detailed follow-up](perf-hitch-followup.md): approved scope, validation, single-baseline table and GPL history.
- [Experiment log](perf-log.md): prefetch/H1/pacing history, pair probe/implementation/failed-RSS acceptance, subsequent user override and release smoke/promotion/day-end records.
- [Plan 2](perf-plan-2.md): prior short-route acceptance and lock investigation, distinct from this broader hitch problem.
- [Architecture](ARCHITECTURE.md), root `AGENTS.md`, `.claude/skills/nx2android-dev/` and `.claude/skills/switch-android-port/`: collaboration, source boundaries and device rules.

H1, pacing and the pair probe/implementation/acceptance/override shipment are complete. Current production/restore target is728b8a49; preserve06e382a5 and bcd840c1 plus their data/backup paths. Pair criterion4 remains failed despite the explicit user shipping decision; earlier pacing criterion2 remains separately failed/overridden. Supervisor reviews and merges perf/pair-warm into local main separately; this stage does not merge, push, bump versions or publish. Remaining work concerns unwarmed line/integer variants and first compiles, cache/memory growth/attribution, admission coverage and controlled workload comparisons. Prediction has no measured benefit. Any next observation first needs relevant upstream review, a serial-critical-path hypothesis and a new bounded user-approved protocol. Begin further work in a new supervised session; unlock stays manual and no credentials are stored.
