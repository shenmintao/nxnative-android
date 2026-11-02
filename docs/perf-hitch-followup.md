# BotW user-play hitch follow-up

Authorized by the user on 2026-11-02 after the manual-play report and upstream review.

For a standalone review brief with version identities, raw-artifact locations and specific code questions, see the [AI handoff](ai-handoff-botw-hitches.md).

## Current pacing correction and user-approved production, 2026-11-03

Candidate2 (`4a6b5c1e`) keeps pacing enabled and separates its scheduling estimate from raw `vk_gpu_ms_recent()` timing. With at least three known spans (up to eight), detect `latest > max(2*median, game_period)` and replace only that outlier with the median. Raw timing/ADPF, the game-period cadence,4 ms floor, shaders, PPE2 ABI and prefetch/prediction are unchanged. `DM_VK_PACE_BUDGET` defaults ON; debug can toggle it live, and cumulative trim counts/ms are appended to the existing600-frame display-timing line. The debug injection is excluded from release.

| Fixed-view acceptance | Candidate1: cap replacement / debug2 | Candidate2: median replacement / debug3 |
|---|---:|---:|
| OFF / ON second gaps | 12/12 /6/12 | 12/12 /0/12 |
| OFF / ON median excess | 345.875489 /216.706390 ms | 341.708864 /208.373140 ms |
| ON/OFF ratio, required<=0.6 | 0.626545: fail | 0.609797: fail |
| Criteria0/1/2/3/4 | pass/fail/fail/pass/pass | pass/pass/fail/pass/pass |

**The pre-declared acceptance test did not pass.** Candidate2 exceeds its205.025319 ms ON limit by3.347822 ms. After reviewing this miss, the user explicitly accepted candidate2 and authorized local release smoke and production promotion. This shipping decision is an override, not a changed threshold or a retrospective test pass. Both ON phase medians are below both OFF phase medians; ordinary cadence passes. V233104 is reused without rerun because only arithmetic changed; errors0 and exactly three unused-output baseline warnings match182026, with no regression RED. Full tables and supplementary requested-period estimates/bounds are in `BENCH/pace-budget-validation.json`; those supplementary metrics do not affect acceptance.

Candidate2 B `20261103-002509-pace-budget-C2-B`:59.371859 fps, late>17.5 ms1.361021%,p99 25.001354 ms,>50/>100 counts6/3,max166.675886 ms, second gaps0 and total>50 ms excess466.697867 ms. B passes second gaps<=1 and late<=1.8%; candidate1 had1/1.506830%, historical182822 had6/1.112990%. This is a single historical route comparison with differing draw/compile work, not a general FPS-gain claim. A/B sample internal1600x900, output1080p, balanced caps1996800/2438400 kHz and severity0; screen-on idle cooling with three zero-severity observations is the approved deviation.

Release smoke `20261103-004043-coverage` uses an isolated owned seed:30 s gameplay, manual save and reload of the newly written slot, stable PID4685, RED0,1209 prewarm keys and38 display-timing checkpoints with `budget_on=1`. The isolated `save/2/game_data.sav` changes; all34 primary-save hashes and original env/settings hashes remain unchanged. Production is `pace-budget-release3.apk`, SHA-256 `06e382a5ecab44af72236bba948f14544c74a2e8089f8ec9fc44f293689b041c`, also exported to the existing private `ROOM/ZeldaBreathOfTheWild.apk`. App stopped/display OFF; data/modules and app version are unchanged.

Rollback retains `BENCH/prefetch-rc2-release.apk` and `ROOM/ZeldaBreathOfTheWild-prefetch-bcd840c1.apk`, both SHA-256 `bcd840c1a9a3427e6dbe4f46c5caa041d6caf5f9e0fb941243e2ca42ddcc988d`; the existing data path and earlier rollback backups remain intact. `BENCH/pace-budget-promotion.json` indexes the decision, installed/exported/rollback identities and recovery evidence in group20261103-004026. `current-production.json` and `production_runtime.py` select hash-verified release3 in the shared harness constructor and restore path, overriding stale historical defaults; an offline mocked restore verifies this without device actions. Historical scripts still require inspection and new authorization before use.

Pacing6/6 and full source162=159 passed/3 optional native Yaz0 skips; debug3/release3 each build144 sources. Next: first synchronous pipeline-creation stalls remain the main open hitch source; prediction has no measured frame-time benefit. No third pacing candidate, further device experiment, version bump, release notes, GitHub release, push or merge is authorized here; supervisor reviews and merges local main separately.

## Scope and acceptance

The following scope and implementation history predates the current pacing production above; its earlier restore targets are historical identities.

- BotW 1.9.0 (v1114112), TB323FU / SM8850P / Adreno 840 / Android 16, balanced mode.
- Docked 1920x1080 output, stock 1280x720..1600x900 internal DRS, target 60, HOLD off. No quality, vendor mode, or system performance changes.
- Use an isolated copy of the verified owned Great Plateau seed. Expand the previous one-second forward/back route to bounded six-second moves and camera turns; record screenshots and internal resolution. Never play the primary save.
- First diagnose one 120-second route after 60-second warmup: align actual present intervals with timed graphics/compute pipeline creation, texture work, submission waits, and GPU segment/gap observations. Diagnostic trace overhead is recorded and its fps is not an acceptance result.
- Implement only a cause shown to explain a material portion of long frames. Check the two upstreams first. Do not skip draws or alter shader precision.
- The user explicitly waived OFF/ON/ON/OFF for this follow-up on 2026-11-02. Build on production 44a337e0, validate source checks, unchanged rendering/input and zero Vulkan errors, then run one bounded moving smoke in the owned seed. Record presentation metrics as observations only; performance gain remains unquantified. Do not restart comparative or long-running performance loops.
- Finish with appropriate unit checks, zero Vulkan validation errors, release smoke, original preferences and unchanged primary saves. Those implementation groups restored44a337e0 and their release smoke promoted bcd840c1. That baseline is now superseded by release3 above; future groups restore current production through the registry. Final30-minute soak remains waived unless the user asks for it.
- After rc2 packaging, the user requested one original-version measurement against the existing new-build observation. Run the retained pre-prefetch debug F416 (production equivalent 44a337e0), the same owned seed, driver/key cache, six-second route, balanced mode and 1080p output / stock internal DRS, 60 s warmup plus 120 s actual-present capture. Compare fps, >17.5 ms / >50 ms / >100 ms intervals, maximum interval, sampled main-pass size and thermal/cap observations. Restore production bcd840c1 and verify unchanged primary saves/preferences. This authorizes one baseline, not a restarted four-window loop; a single historical pair does not satisfy repeatability acceptance.
- The user subsequently approves the proposed H1 diagnostic: one 120 s moving run after 60 s warmup using the same F416 APK/cache/seed/route as original run192400, with only `DM_VK_PACE=0` added. Compare consecutive >50 ms second gaps and their excess over16.667 ms, plus >100 ms count, compile-demand time, mean/p99, resolution and health. Supporting signature: material first compile stalls remain while second-gap count/excess falls at least50% from the original five /741.7 ms; without material first stalls the outcome is inconclusive. This is a diagnostic, not permission to ship pacing disabled or a repeated acceptance loop. Restore current bcd840c1, preferences and primary saves.

## Starting evidence

Manual route diagnostic `20261102-151653-user-live-hitch`: 120.112 s, 58.004 fps, 33 intervals >50 ms, 12 >100 ms, max 241.680 ms. The periodic pipeline counter rises 1301 to 1689 after prewarm completed. The old graphics demand path increments `pipes_built` once inside `build_pipeline` and once again in `graphics_pipeline`: +388 counter units correspond to approximately 194 demand builds, not 388 distinct creations. Android generic `mtl_*` pipeline/upload statistics are fixed-zero stubs and cannot exclude this work; GPU timestamp envelopes include submission gaps.

Publication and Dimensity 9400 experiments were held during this investigation. The first-use mechanisms below form candidate 0.3.0rc2; Dimensity experiments remain deferred. The accepted short-route plan 2 measurements stay in `perf-plan-2.md`; they are not evidence for the broader route.

## Earlier prefetch direction

The user approved program-creation shader prefetch and bounded upcoming-command pipeline compilation, then waived the four-run performance comparison. Implement these on the production-derived branch, with ordinary full pipelines and correct synchronous fallback whenever preparation is incomplete. The failed GPL experiment is checkpointed separately at `experiment/gpl-resident-20261102` / `16600918`; it is excluded from this implementation. Keep balanced mode, stock DRS, save isolation and the production rollback procedure above.

## Implemented prefetch and prediction

- Source adapts nxnative `c646d43`'s program-creation prefetch and bounded pending/predicted builds to existing Vulkan pipelines. The queue owns shader keys; speculative jobs own complete pipeline keys and stable shader/layout handles. The actual draw recomputes its key, steals queued work or waits for an exact running build. It never skips a draw for this optimization.
- Prediction uses a private replay-state copy, allows at most two admitted builds and eight bounded scans per logical submission, follows nested calls/jumps, and stops at changed targets or memory/compute/synchronization work. Uncertain shader-view variants may miss the prediction and compile normally. The existing 1000-byte PPE2 key ABI is unchanged.
- 156 source tests passed. Cold first-use Vulkan run `20261102-182026-coverage`, with recorded prewarm disabled, passes fixed/moving checks: zero validation errors, three unused-output warnings, 3264 shader fetches completed; 44 predicted jobs = 42 completed + two stolen by demand. All three compiler worker masks are 0-7, rather than the replay thread's inherited narrow affinity. Stock DRS selects 1280x720 under validation overhead; not performance evidence. An earlier attempt was interrupted by a transient read-only adb timeout, recovered to 44a337e0, and repeated with retry inside the original scene deadline.
- Ordinary single observation `20261102-182822-prefetch-moving-observation`: 60 s warmup, 120.047 s moving, 7099 presents, 59.135126 fps, late >17.5 ms 1.112990%, p99 25.001354 ms, >50/>100 ms 16/5, max 200.010937 ms. Main pass 1600x900 before/after, output 1920x1080, balanced ceilings 1996800/2438400 kHz, sampled CPU/GPU/skin severity zero, audio and PID continuity pass. End RSS 2405240 KiB (~2.29 GiB). Raw timing records 46 graphics demand misses (including prediction waits), not 46 driver compiles; background work is separate. This is not a controlled improvement result and remaining hitches are evident.
- Release `bcd840c1a9a3427e6dbe4f46c5caa041d6caf5f9e0fb941243e2ca42ddcc988d` passes isolated save/load and 30 s gameplay smoke in `20261102-190024-coverage`, with packaged 1209 prewarm keys and RED=0. It is installed as the new production runtime; original preferences and primary saves are unchanged, app stopped/display OFF. The previous 44a337e0 APK and data remain available for rollback; rewritten modules and shader data are unchanged.
- Candidate version is 0.3.0rc2. The 30-minute soak and comparative FPS windows stay waived; performance gain is unquantified. Desktop packaging uses an isolated venv and a locally built bootloader, with no global Python launcher changes.

## Desktop candidate verification

Windows x64 bundle built with Python 3.12.10, PySide6 6.12.0 and PyInstaller 6.22.3 in an isolated environment. The PyInstaller source archive matches the SHA-256 published by PyPI; the launchers were compiled locally. Frozen `--help`, `profiles`, `-m porter profiles` and engine `build.py --help` pass. The native Windows Qt platform renders all four tabs with the expected 0.3.0rc2 version and adjacent engine path; the hidden-window screenshot was inspected. An offscreen-platform font artifact does not occur with the native Windows platform. No game assets, keys or toolchains are bundled.

Final source review moved the single-owner replay shadow off guest stacks and kept unsupported stages on the original demand path. The 12 affected queue/replay/host checks were rerun successfully, all 144 release sources build, and the resulting bcd840c1 APK passed the isolated release smoke above. The earlier cold validation and ordinary moving observation used the preceding build of the same two mechanisms.

## Requested single original-version baseline

After packaging, the user requested one run of the pre-prefetch implementation. Retained debug F416 (production equivalent 44a337e0) ran in `20261102-192400-original-pre-prefetch-once`, with the same owned seed, cache inputs, route script, timing switches, 60 s warmup and 120 s capture as the existing new-build observation. Both sample internal 1600x900 before/after, output 1920x1080, prewarm1209, balanced CPU caps 1996800/2438400 kHz and CPU/GPU/skin severity zero. The start screenshots match location/view; the script does not lock weather or simulation phase, and periodic mean draw counts differ (4450.417 original, 3706.750 new).

| Metric | Original baseline | Earlier new-build observation |
|---|---:|---:|
| Average fps | 58.934487 | 59.135126 |
| Late intervals >17.5 ms | 1.300537% | 1.112990% |
| p99 interval | 25.001354 ms | 25.001354 ms |
| Intervals >50 ms | 14 | 16 |
| Intervals >100 ms | 6 | 5 |
| Maximum interval | 308.350416 ms | 200.010937 ms |

The +0.200639 fps (+0.34%) difference is inside the ~1% noise floor. The p99 is unchanged; maximum interval and >100 ms count are lower, while >50 ms count is higher. This single historical pair provides no clear performance improvement and is not repeated acceptance. Original audio/PID/presentation gates pass with 127 latency matches and RED0. Then-current production bcd840c1, original preferences and primary saves were verified/restored; app stopped/display OFF. No additional comparison windows or code/default changes followed this requested measurement.

## Approved H1 pacing-off diagnostic

After the offline review, the user approved one120 s diagnostic. Run `20261102-211447-H1-pace-off-once` uses the exact F416 APK, cache inputs, owned seed and route from the original192400 run, with60 s warmup. After excluding private isolation paths, `DM_VK_PACE=0` is the only environment difference. Logs confirm pacing is disabled before the window.

| Metric | Original pacing on | Diagnostic pacing off |
|---|---:|---:|
| Average fps | 58.934487 | 59.498442 |
| Intervals >17.5 ms | 1.300537% | 8.695043% |
| p99 interval | 25.001354 ms | 25.001458 ms |
| Intervals >50 / >100 ms | 14 / 6 | 10 / 3 |
| Maximum interval | 308.350416 ms | 158.342031 ms |
| Consecutive second >50 ms gaps | 5 | 0 |
| Second-gap excess above16.667 ms | 741.712292 ms | 0 ms |
| Total >50 ms excess above16.667 ms | 1508.429376 ms | 733.382913 ms |
| Timed graphics demand records / wall time | 51 / 945.985879 ms | 47 / 838.435050 ms |
| Mean draws in periodic samples | 4450.417 | 4449.167 |

Six demand clusters of at least50 ms remain in the off run; disappearance of the second gaps is not explained by the absence of first-use stalls. This meets the predeclared H1 diagnostic signature, supporting pacing amplification. It is one historical pair, with differing compile work and no deterministic game replay, rather than proof of a production fix or general FPS gain. The large increase in ordinary late intervals makes pacing-off unsuitable as the retained solution.

Both sample internal1600x900 before/after, output1920x1080, prewarm1209, identical balanced CPU caps and CPU/GPU/skin severity0. Audio/PID/trace gates pass with126 latency crosschecks and RED0. `BENCH/H1-pace-off-comparison.json` retains the rows, pair intervals, compile-demand evidence and matching-condition checks; helpers are `pace_off_once.py` and `summarize_pace_off.py`. Then-current production bcd840c1, original preferences and primary saves were verified/restored, app stopped/display OFF. The diagnostic promoted no change. Its proposed pacing-budget correction was subsequently implemented, tested and shipped by user override as recorded above.

## Previous GPL experiment

The diagnostic capture above is complete. The user stopped the route-list-only proposal because unvisited routes would still compile, then approved the resource-derived graphics-pipeline-library prototype. The static scan finds 15,767 known shader pairs and full pair coverage of both sampled runs; this is not an exhaustive list of complete pipelines or runtime-generated shaders.

The Adreno 840 reports graphicsPipelineLibrary=1 and fastLinking=1; the recorded Mali driver has neither extension. The prototype is opt-in (`DM_VK_GPL=1`), combines the actual pre-rasterization/fragment modules and descriptor layout in a library, and links vertex/attachment state at first use. No draws are omitted. State and shader variants not covered by prewarm still compile synchronously. Validate rendering first, then measure full-table startup cost/memory and the same OFF/ON/ON/OFF route; advertisement or a simple probe is not acceptance.

Initial game library creation failed with VK_ERROR_UNKNOWN and zero validation errors. An independent real-shader probe reproduces the failure while complete pipelines succeed. A synthetic shader fails when only generator word 2 becomes zero; the real pair succeeds when only that word becomes 1 (preserving its tool ID). Descriptor sets/buffers, independent-set layouts, SPIR-V version changes and ordinary optimization did not explain the failure. The runtime now applies the nonzero low version bits only to zero-generator modules while GPL is enabled; shader instructions are unchanged. [SPIR-V physical layout](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#_physical_layout_of_a_spir_v_module_and_instruction) permits zero and assigns the generator word no semantics.

Strict game validation `20261102-171648-coverage` passed stationary/moving checks: 962 libraries, 1,212 links, zero creation failures, zero validation errors, three unused-output warnings. Stock DRS selected 1280x720 under validation overhead; this is not performance acceptance. The private runner now archives/removes a stale log before prototype startup so an earlier RED cannot fail a new process before its log rotates. All 154 source tests passed across the full run (151 passed, three decoder tests subsequently passed with the native DLL configured).

The full resident-table probe `20261102-172313-coverage` did not complete: progress passed 15,000 pairs / 62,000 libraries, then SIGABRT at roughly 587 seconds of app lifetime. Last sampled RSS was 6,296,796 KiB, available system memory 3,292,816 KiB. Android records EXIT_SELF/status134, not LOW_MEMORY; high memory is observed but not an established cause of the abort. Production 44a337e0, original settings and primary-save hashes were restored, app stopped/display OFF. No performance A/B ran and no optimization was promoted. The all-resident prototype is unsuitable for release in this state.

## Targeted upstream gaps reviewed after the user's background-CPU question

Latest fetched references: nxnative `c646d43d567fbd1b96777ff7f3bd6d9aabb1a36a`, nx2android `8e030b41` (already an ancestor of this branch). These are Metal-backend mechanisms without corresponding Android Vulkan paths, not a list of unmerged Android commits:

- `mtl_prog_prefetch`: start shader-library loading when the game creates the program.
- `pso_predict_rest`, `shader_prefetch_rest`, bounded demand-first pending builds: inspect upcoming draws and overlap exact pipeline builds with current replay.
- Metal 4 archived bases and `hybrid_get`: draw with a compatible vertex-pulling pipeline while the specialized pipeline compiles. Android GPL remains an isolated, incomplete experiment; Metal APIs cannot be copied into Vulkan.
- `sdx_decide` / store lookahead, readable-tile pass fusion, and `DM_NVN_CS_HOIST`: reduce attachment traffic and pass splits after dependency checks. Potential Android/Mali applicability is not measured.
- Generic `mtl_create_*` / upload statistics still return zeros in Android `gfx_common.c`; backend timings exist separately but are not wired into these host diagnostics.

The first two mechanisms are implemented above. The shared lock-poll rule, ADPF changes, memory lookup changes, real motion input and `DM_HITCH_DUMP` are already present and must not be reported as missing. Other mechanisms remain outside this candidate's scope; retain the failed resident-table evidence and the ordinary production baseline.
