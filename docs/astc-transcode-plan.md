# BC to ASTC: offline feasibility, 2026-11-03

Step 1 only, based on `b61cdce8`, branch `perf/astc-transcode`. No device commands,
game launches, APKs, runtime/default/profile changes, installation-gate changes,
production changes, merge or push. The supplied upstream investigation found no
transcoder to port; the optional repeat search was not needed for these prototypes.

**Result:** the guarded BC1/BC4 direct paths meet the one-byte error bound in the
reference-decoder sample. An unconditional eight-weight BC4 path cannot do so for
some real blocks. ASTC-only re-encoding fails the 38 dB requirement, even at
thorough: 46 unique payloads fail RGB or alpha, increasing to 50 when BC5's two
meaningful channels are checked without diluting their error with constant blue.
No implementation or quality-threshold change is approved by this report.

## Inputs, coverage and reproducibility

- Owned BotW 1.9.0 / v1114112 merged RomFS and DLC 1/2, as identified by the
  existing private data-pack manifest. All 48,157 files / 16,519,555,942 source
  bytes scanned in 130.583 s; zero parse/decompression errors.
- Reused the porter's `shader_split.py` SARC/Yaz0 readers and existing native
  Yaz0 decoder. BNTX headers and mip tables are parsed by the private tool;
  block addressing is extracted unchanged from runtime `nvn_deswizzle.c`.
  A separate DLL built from unchanged `nvn_tex.c` and `nvn_deswizzle.c` confirms
  all 37,816 BC textures / 1,318 shapes: zero storage-size, tile or physical
  mip-offset mismatches. All are 2D, including 43 array textures; all use block
  linear storage. All layers and mip levels are included in the counts below.
- 16,476 unique format+BC-payload hashes. Container duplicates count separately
  in the inventory; quality distributions count unique payload representatives
  equally. Five duplicate payloads have different view swizzles, and one BC4
  payload has different geometry. All re-encoded BC3/BC5 payloads have consistent
  geometry; the BC4 alias is covered by the block-based direct/fallback sample.
- `BENCH` means the existing private `.../01007EF00011E000_build/android/bench`.
  Everything except this report and the perf-log entry remains under
  `BENCH/astc-transcode/`: extracted bytes, C/C++ prototypes, tools, CSVs, banks
  and decoder sources. No texture bytes or texture names are in this report.

Reproduction tools, in order: `fetch_astcenc.py`, `build_codec.py`,
`build_native.py`, `census.py --workers 6`, `verify_geometry.py`, `test_exact.py`,
`run_proof.py`, `quality.py --workers 4`, `benchmark.py`, `package_probe.py`,
`summarize.py`, `finish.py`. See the private README for paths and commands.
`manifest.json` hashes tools/results and the unchanged runtime sources.

## Census

Logical block bytes exclude BNTX alignment/padding. The BNTX format identifies
BC1 UNORM/sRGB, **not the later NVN RGB/RGBA binding**; an exact RGB-versus-RGBA
binding census is unavailable offline. Both are included in the BC1 rows.

| Source format | Textures | 4x4 blocks | BC bytes |
|---|---:|---:|---:|
| BC1 UNORM, RGB/RGBA undifferentiated | 11,747 | 142,191,280 | 1,137,530,240 |
| BC1 sRGB, RGB/RGBA undifferentiated | 16,143 | 741,334,243 | 5,930,673,944 |
| BC2 UNORM / sRGB | 0 / 0 | 0 | 0 |
| BC3 UNORM | 215 | 136,935 | 2,190,960 |
| BC3 sRGB | 1,226 | 39,403,132 | 630,450,112 |
| BC4 UNORM | 5,399 | 109,518,423 | 876,147,384 |
| BC4 SNORM | 3 | 4,101 | 32,808 |
| BC5 UNORM | 2,919 | 20,634,394 | 330,150,304 |
| BC5 SNORM | 164 | 648,560 | 10,376,960 |
| BC6H UF / SF | 0 / 0 | 0 | 0 |
| BC7 UNORM / sRGB | 0 / 0 | 0 | 0 |
| **Total** | **37,816** | **1,053,871,068** | **8,917,552,712** |

BC4/BC5 have UNORM/SNORM formats, not sRGB variants; BC6H is floating point.
BC2, BC6H and BC7 are absent in this complete BNTX census, so their game quality,
encoding throughput and HDR metrics are **N/A**, not passed or inferred from BC3.
This does not prove that dynamically generated or non-BNTX resources never use them.

BC1 content proxies, not binding classifications:

| Content | Textures | Blocks | BC bytes |
|---|---:|---:|---:|
| UNORM, no transparent selector used | 11,419 | 137,890,665 | 1,103,125,320 |
| UNORM, transparent selector used | 328 | 4,300,615 | 34,404,920 |
| sRGB, no transparent selector used | 13,399 | 688,118,173 | 5,504,945,384 |
| sRGB, transparent selector used | 2,744 | 53,216,070 | 425,728,560 |

Transparent-selector content would require punch-through if bound as BC1 RGBA;
a BC1 RGB binding instead forces alpha to one. Opaque content can still be
bound as RGBA. The prototype tests the stricter RGBA reference, preserving the
option to force alpha to one for RGB views.

| Entire inventory, all mips/layers | Bytes | Meaning |
|---|---:|---|
| Source BC logical payload | 8,917,552,712 | 8 or 16 bytes/block |
| Source BNTX image storage | 9,305,645,056 | Includes padding |
| All ASTC 4x4 | 16,861,937,088 | 16 bytes/block |
| Uniform RGBA8 estimate | 67,430,873,360 | Four bytes per actual texel |
| **Actual current CPU-decoded formats** | **61,495,964,017** | RGBA8, R8/S8 and RG8/S8 as appropriate |
| Unique BC / ASTC payloads | 5,235,060,320 / 10,119,365,888 | Content deduplicated |
| Unique uniform RGBA8 / actual decode | 40,470,289,072 / 35,928,376,205 | Content deduplicated |

These are catalog footprints, **not simultaneous residency or predicted RSS**.
Current `bc_target()` uses R8 for BC4, RG8 for BC5, and RGBA16F for BC6H.
ASTC doubles BC1/BC4 block storage, matches BC3/BC5, quarters BC1/BC3 RGBA8,
and approximately halves BC5 RG8. BC4 ASTC has no general saving over current
R8, and can be larger at small mip edges. Guest BC memory remains separately
present, so host images/sidecars do not replace the game's guest allocation.

Of 1,635 unique BC5 payloads, 663 have normal-map name evidence. Names are a
heuristic, not proof; the angular statistics separately identify that subset.
SNORM is real: 3 BC4 and 164 BC5 inventory entries. Signed endpoint comparison
finds 150/4,101 BC4 SNORM six-value blocks; unsigned endpoint comparisons are
not used for that statistic.

## BC1/BC4 C prototype and the one-byte gate

`bc_astc.c` is compiled as C11 alongside the repository's precise `bcdec`.
The independent reference is unmodified Arm astcenc at
[`d0f91386350222fcbf6da533c0f7a3f9b5c6b783`](https://github.com/ARM-software/astc-encoder/tree/d0f91386350222fcbf6da533c0f7a3f9b5c6b783).
The 128-bit blocks use the specified ASTC endpoint/weight representation;
reference block-info checks confirm partition count, grid, endpoint mode and
quantization. Zero malformed blocks were found. See the
[Khronos format specification](https://registry.khronos.org/DataFormat/specs/1.3/dataformat.1.3.html)
for the decoding rules.

- BC1 four-colour: one partition, RGB direct endpoints (CEM 8), 8-bit endpoint
  quantization, 4x4 grid/four weight levels, block mode `0x42`. BC selectors
  0/1/2/3 map to weights 0/64/21/43. Endpoint ordering avoids ASTC's blue
  contraction; swapping endpoints reverses weights. Naively copying 565-expanded
  endpoints can differ by 2/255. A bounded per-channel endpoint adjustment of
  up to two byte values reduces the maximum to 1/255.
- BC4 eight-value: luminance direct (CEM 0), 8-bit endpoints, 4x4 grid/eight
  levels, mode `0x53`. Weights are 0/9/18/27/37/46/55/64. The same bounded
  endpoint search is guarded by a maximum-error check. Rejected blocks go to
  encoding fallback; they are never labelled exact-path successes.
- BC1 three-colour/punch-through and BC4 six-value blocks use decode + astcenc
  fallback. No unmeasured partition-seed matching claim is made. SNORM bypasses
  this direct path. C fallback output is explicitly a placeholder; `quality.py`
  supplies the actual measured encodings.

Sample: all 14,066 unique BC1/BC4 payloads, up to 512 uniformly chosen blocks
without replacement per payload, PCG64 seeded by its payload hash. Includes
6,435,299 blocks across all mip/layer populations, including 512 signed BC4
blocks. Small payloads are fully tested. The table gives **sample** counts;
full inventory mode fractions follow separately.

| Format | Sample blocks | Accepted direct blocks | Naive maximum, bytes | Guarded maximum, bytes | Guarded violations |
|---|---:|---:|---:|---:|---:|
| BC1 UNORM | 1,464,408 | 1,277,900 | 2 | **1** | 0 |
| BC1 sRGB | 3,791,625 | 2,895,751 | 2 | **1** | 0 |
| BC4 UNORM | 1,178,754 | 890,658 | 2 | **1** | 0 |
| BC4 SNORM | 512 | 0 | N/A | N/A | Bypassed |

The accepted direct subset **passes <=1/255 per channel**. Alpha is exact on
those paths. This is a byte-space bound, not lossless encoding. LDR UNORM uses
reference **UNORM8 decode mode**; sRGB uses the ASTC sRGB profile's eight-bit
colour decoding. Later Vulkan integration must explicitly enable
`VK_EXT_astc_decode_mode` and request `R8G8B8A8_UNORM` for the LDR views to match
this measurement. That setting has no effect on sRGB views and is unsuitable
for HDR blocks. Current runtime does not yet enable that extension.
[Vulkan decode-mode rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewASTCDecodeModeEXT.html)

Supplementary exhaustive arithmetic: all 5,120 scalar 565 endpoint combinations
(32x32 plus 64x64), separately for UNORM and sRGB and all four palette values,
have adjusted maximum <=1. This complements, rather than replaces, the independent
ASTC-decoder sample of combined RGB blocks.

BC4 caveat: 6,693 of the 897,351 sampled eight-value blocks still exceed one byte.
For 32 distinct real counterexample endpoint pairs, exhaustive search of all
256x256 ASTC luminance endpoint pairs, allowing every texel its nearest of all
eight weights, still has best maximum **2/255**. Thus the prescribed one-partition,
eight-weight BC4 mode cannot unconditionally meet the bound; a fallback is needed.

| Full inventory fallback reason | Blocks / total | Share |
|---|---:|---:|
| BC1 UNORM, three-colour | 14,463,828 / 142,191,280 | 10.1721% |
| BC1 sRGB, three-colour | 154,831,069 / 741,334,243 | 20.8855% |
| BC4 UNORM, six-value | 23,178,289 / 109,518,423 | 21.1638% |
| BC4 UNORM, additional eight-value quantization guard | Estimated 469,049 | Estimated 0.4283% of all BC4 UNORM blocks |
| BC4 SNORM | 4,101 / 4,101 | 100% |

Mode counts are exact over the complete inventory. The extra BC4 estimate weights
each sampled payload's rejection rate by its full block count and multiplicity;
it is not an exhaustive count. Combined estimated BC4 UNORM fallback is 21.5921%.

Actual fallback encodings, PSNR median/p5 in dB over per-payload sampled blocks:

| Fallback | Preset | RGB med / p5 | Alpha med / p5 | Maximum RGB / alpha error, byte units |
|---|---|---:|---:|---:|
| BC1 UNORM | fast | 67.33 / 58.41 | inf / inf | 120 / 64 |
| | medium | 67.47 / 59.30 | inf / inf | 97 / 36 |
| | thorough | 68.49 / 59.89 | inf / inf | 59 / 36 |
| BC1 sRGB | fast | 67.50 / 54.20 | inf / 67.12 | 75 / 51 |
| | medium | 67.90 / 57.09 | inf / inf | 49 / 48 |
| | thorough | 70.13 / 58.33 | inf / inf | 46 / 48 |
| BC4 UNORM | fast | 59.45 / 53.94 | inf / inf | 11 / 0 |
| | medium | 64.49 / 59.47 | inf / inf | 7 / 0 |
| | thorough | 71.84 / 61.21 | inf / inf | 8 / 0 |

No fallback payload sample falls below 38 dB in RGB/alpha; BC4 meaningful-R p5
is 49.17 / 54.70 / 56.44 dB. Fallbacks **do not** meet the direct one-byte bound;
large isolated colour/alpha errors remain despite high average PSNR. A full
offline conversion must quality-check whole images, including alpha cutout edges.

## Re-encoded formats: quality fails the universal 38 dB gate

All unique BC3/BC5 payloads and BC4 SNORM: every layer/mip, with valid texel masks
and replicated mip edges during encoding. The encoder packs independent 4x4
blocks into an atlas; atlas padding is excluded from PSNR and cropped from banks.
All three presets use the same inputs: fast=10, medium=60, thorough=98.
No malformed ASTC blocks in 48,648 payload encodes across this and the fallback tests.
Total four-worker quality-run wall time: 384.622 s, including IO, decoding,
three presets, reference decoding, metrics and output writes. This is not a
timing for converting the complete game's BC1/BC4 populations.

PSNR is measured against the repository bcdec result, RGB averaged separately
from alpha; sRGB's declared table is in encoded byte space. Signed channels
use full range two as PSNR peak. `inf` means zero squared error. Distributions
give per-payload median and nearest-rank fifth percentile, not a global MSE.

| Source, unique payloads | Preset | RGB med / p5, dB | Alpha med / p5, dB | RGB or alpha <38 |
|---|---|---:|---:|---:|
| BC3 UNORM, 154 | fast | 42.93 / 39.10 | 43.95 / 36.21 | 19 |
| | medium | 51.04 / 47.14 | 53.93 / 39.33 | 3 |
| | thorough | 51.49 / 47.88 | 55.11 / 39.70 | 3 |
| BC3 sRGB, 621 | fast | 50.50 / 40.01 | 47.29 / 36.40 | 53 |
| | medium | 53.37 / 42.21 | 48.58 / 36.78 | 42 |
| | thorough | 53.66 / 42.42 | 48.99 / 37.14 | 38 |
| BC5 UNORM, 1,603 | fast | 51.75 / 42.11 | inf / inf | 6 |
| | medium | 54.03 / 44.12 | inf / inf | 4 |
| | thorough | 54.44 / 44.28 | inf / inf | 4 |
| BC5 SNORM, 32 | fast | 42.90 / 36.25 | inf / inf | 4 |
| | medium | 44.69 / 38.11 | inf / inf | 1 |
| | thorough | 44.79 / 38.16 | inf / inf | 1 |
| BC4 SNORM, 1 | fast | 61.45 / 61.45 | inf / inf | 0 |
| | medium | 61.97 / 61.97 | inf / inf | 0 |
| | thorough | 62.14 / 62.14 | inf / inf | 0 |

Meaningful-channel check: BC5 UNORM RG med/p5 is 49.99/40.34, 52.27/42.36,
52.68/42.52 dB; BC5 SNORM is 41.14/34.49, 42.93/36.35, 43.03/36.40.
At thorough, 6 UNORM and 3 SNORM BC5 payloads miss 38 dB in RG. Total failures
are fast 82 / medium 50 / thorough 46 by requested RGB/alpha, or **99 / 54 / 50**
including meaningful R/RG. The complete private lists are
`quality-below-38.csv`, `quality-exceptions.csv`, and expanded catalog aliases
in `quality-exception-aliases.csv`. No exception is hidden by reporting medians.

Supplementary linear-light BC3 sRGB RGB med/p5: fast 49.89/39.85, medium
52.52/42.74, thorough 52.93/43.04 dB. Alpha still fails; changing metric space
does not make the candidate pass.

BC5 is encoded in astcenc normal-map mode as RRR plus G in alpha, then viewed
as `(R,A,0,1)`. Normal angles reconstruct positive Z as
`sqrt(max(0,1-x*x-y*y))` and renormalize. This is a hypothesis for BC5 normal
usage, not a claim that all BC5 data is a positive-Z normal map.

| Source | Preset | Texel-weighted angle median / p95 | Name-evidence subset median / p95 |
|---|---|---:|---:|
| BC5 UNORM | fast | 0.01 / 1.50 degrees | 0.00 / 2.08 degrees |
| | medium | 0.00 / 1.07 | 0.00 / 1.62 |
| | thorough | 0.00 / 1.04 | 0.00 / 1.62 |
| BC5 SNORM | fast | 0.34 / 4.74 | 0.34 / 4.74 |
| | medium | 0.31 / 3.76 | 0.31 / 3.76 |
| | thorough | 0.31 / 3.73 | 0.31 / 3.73 |

Histogram resolution is 0.01 degrees. Flat/unchanged texels dominate some medians;
p95 and private per-payload statistics remain necessary.

BC6H HDR: no source textures, hence no measured HDR PSNR or accepted HDR path.
Any later test must compare float output with bcdec, report linear relative
RMSE and log-luminance/stop error over the actual HDR range, and test negative
SFLOAT separately. The pinned astcenc regular LNS conversion maps negative
inputs to zero; do not silently apply it to arbitrary signed BC6H. Retain the
existing RGBA16F fallback until signed/HDR coverage is demonstrated.
[Encoder conversion source](https://github.com/ARM-software/astc-encoder/blob/d0f91386350222fcbf6da533c0f7a3f9b5c6b783/Source/astcenc_vecmathlib.h)

## PC cost, package prototype and phone estimates

Host: i7-13700H, 14 cores/20 logical processors, Windows x64, GCC `-O3 -mavx2
-mf16c -mpopcnt`; unmodified astcenc library, one context/thread at a time.
Up to 65,536 real blocks from deterministic per-payload samples, one untimed
call then three timed calls; table uses the median. Decode, context creation
and file IO are excluded. Frequency/power/affinity are not controlled. The
four-worker quality-run call-time sums are not substituted for these serial
throughputs.

| Source | fast / medium / thorough, MPix/s | Assumed phone thorough seconds per 1024x1024 |
|---|---:|---:|
| BC3 UNORM | 3.270 / 1.901 / 0.677 | 6.19-15.49 |
| BC3 sRGB | 3.974 / 1.697 / 0.825 | 5.08-12.71 |
| BC5 UNORM | 2.506 / 0.966 / 0.407 | 10.32-25.79 |
| BC5 SNORM | 0.747 / 0.368 / 0.219 | 19.17-47.93 |
| BC4 SNORM | 17.074 / 3.911 / 0.204 | 20.55-51.37 |
| BC1 UNORM fallback | 9.586 / 4.458 / 1.461 | 2.87-7.18 |
| BC1 sRGB fallback | 19.997 / 9.272 / 2.958 | 1.42-3.55 |
| BC4 UNORM fallback | 16.019 / 3.951 / 1.243 | 3.38-8.44 |

The phone column assumes **10-25% of serial host throughput**, solely as a
conservative planning scenario. It is neither a device measurement nor a
guaranteed bound; NEON, scheduling, memory bandwidth, frequency and contention
are unmeasured. Even the fast BC3/BC5 scenario is roughly 1.1-14.0 s per
1024-square image. Demand-time full astcenc encoding is a hitch risk.

The C direct prototype processes BC1 UNORM/sRGB/BC4 UNORM at 204.6/250.8/202.9
MPix/s in serial 1,048,576-block tests, including rejection checks. This does
not include encoding rejected blocks or guest deswizzling. BC decode itself
takes about 1.7-2.8 ms per roughly one MPix BC3/BC5 benchmark input on this PC.
At the same unverified phone scaling assumption, even direct conversion of a
1024-square image would take roughly 17-52 ms; it is not automatically safe on
the replay thread. Upload bursts and lookup misses require a separate budget.

Estimated complete unique-set serial PC work, combining direct conversion,
full mode counts, sampled BC4 guard rejects and measured encoding rates:

| Preset | BC3/BC5/signed BC4 encode | BC1/BC4 direct | BC1/BC4 fallback encode | Total PC estimate | Assumed phone total |
|---|---:|---:|---:|---:|---:|
| fast | 112.8 s | 40.9 s | 124.0 s | 4.63 min | 18.5-46.3 min |
| medium | 277.9 s | 40.9 s | 301.0 s | 10.33 min | 41.3-103.3 min |
| thorough | 618.9 s | 40.9 s | 945.2 s | 26.75 min | 107.0-267.5 min |

These content-dependent projections exclude extraction, hashing, IO, image-level
quality gating and lossless exceptions; no complete conversion at each preset
was timed. Parallel PC workers can overlap independent images; no linear
multi-core or phone scaling claim is made.

`package_probe.py` builds and verifies an actual sorted SHA-256 index plus flat
ASTC bank: 16,208 entries, 1,037,312 index bytes, 372,695,696 payload bytes,
**373,733,072 bytes total**, SHA-256
`b79d39f9862775309ba7bdcce50ff108f2c8e6ffb1237d2b6a0836e721744ac3`.
Build/IO wall time 175.483 s. All entries round-trip byte-for-byte. Zlib-6 gives
191,186,421 bytes in 14.450 s; a monolithic compressed bank is not randomly
addressable, so production would need independently compressed chunks.

The bank contains complete BC3/BC5/signed-BC4 payloads and **only sampled
fallback blocks** for BC1/BC4 UNORM. It is not the full game's package size.
A complete content-deduplicated all-ASTC sidecar would be approximately
**10.119 GB / 9.424 GiB raw**, before geometry/codec metadata and exceptions,
in addition to original owned game data. No full-bank compression ratio is
claimed from the sample.
The complete unique BC3+BC5 portion alone is 350,754,752 raw ASTC bytes.
Prepacking all BC1/BC3/unsigned BC5 while retaining BC4 and SNORM as their
current decoded formats would need 8,702,351,872 raw sidecar bytes before
metadata/quality exceptions. This is a catalog-derived size, not a built bank.

The Python mmap/binary-search prototype measures 100,000 known-hit lookups in
0.261 s (2.61 us each); PC SHA-256 measures 1,865 MiB/s. These are prototype
host figures, not C runtime/phone costs. Use one hash per logical mip/payload,
not a 32-byte hash on every 8-byte source block. Include source format,
dimensions/layers/mip geometry, target profile, channel convention and encoder
version/settings in the production key; the simpler prototype key hashes just
format plus sampled/full BC bytes. Cache successful lookups against the guest
dirty stamp and invalidate on writes. Account for hashing and IO on misses.

Keeping just rejected ASTC patches needs a mip hash plus block-position table;
whole-image fallback is simpler and avoids mixed image representations. The
50 thorough meaningful-channel exceptions occupy 1,321,360 unique ASTC bytes
versus 4,507,942 bytes in the existing lossless decoded formats: +3,186,582
catalog bytes for those representatives. Including duplicate inventory entries,
the difference is +3,735,872 bytes. These small catalog differences are **not
resident-memory measurements** and do not bound other transient/fallback costs.

A GPU compute encoder is possible as a later project: write 16-byte ASTC blocks
to a storage buffer, then synchronize/copy into a sampled compressed image.
Do not assume compressed storage-image writes are supported. Direct bit packing
is bounded, but astcenc's partition/endpoint/weight search is a much larger
compute port with quality and GPU-contention risks. No GPU encoder or phone
measurement was made; it is not an established solution for misses.

## Integration design, not implementation

| Existing path | Required later work |
|---|---|
| `gfx_vk_tex.c`: `FT`, `vk_fmt`, `bc_target`, `vk_bc_decode` | Select native BC first. Keep separate guest format/block size and host ASTC format/block size; decoded fallbacks remain available. |
| `img_new`, `image_format_policy` | Create ASTC 4x4 sampled/transfer images only with queried format support; use the selected per-image representation consistently across every view and mip. |
| `pkg_upload_`, `initial_content`, `lin_copy` | Deswizzle guest BC using its original layout; look up/transcode into a separate 16-byte-block staging allocation. Re-upload correctly after dirty-page changes. |
| `gfx_vk_exec.c`: `copy_b2t` | Respect guest row/image strides and region offsets; convert copies at replay time or use the sidecar. Recompute staging byte pitches and allocation sizes. |
| `vk_tex_view` and guest view swizzles | Compose the ASTC channel mapping with existing texture/view mappings, rather than replacing guest swizzles. |
| `porter/datapack.py`, install gate | Produce/version a generic sidecar; advertise a tested complete path before accepting a non-BC device. |

The 4x4 block grid is unchanged: mip dimensions and aligned copy regions map
one-to-one in block coordinates. **Byte offsets do not:** BC1/BC4 expand from
8 to 16 bytes/block. Keep source and destination block sizes separate when
calculating pitches, slab/layer offsets, scratch space and copy records. Vulkan
`bufferRowLength`/`bufferImageHeight` use texel dimensions, not bytes. Validate
block-aligned offsets, mip-edge exceptions, 16-byte ASTC staging offsets,
array/cube views, transfers, barriers and compatible mutable views.
Current decoded-BC `copy_b2t` already reports a RED and skips image-to-buffer
readback; ASTC transcoding does not by itself supply BC re-encoding for that
direction. Treat such usage as explicit unsupported coverage to audit later.

Channel/colour policy:

- BC1 RGB/RGBA, BC2/BC3/BC7: ASTC UNORM or sRGB matching the source. Force alpha
  to one for RGB-only bindings. Preserve sRGB interpretation in view/cache keys;
  never treat sRGB bytes as linear-light source during ordinary transcoding.
- BC4 unsigned encoded as ASTC luminance: logical mapping `(R,0,0,1)`, composed
  with guest swizzles. Prefer the current R8 lossless path if memory is the goal.
- BC5 unsigned encoded RRR/G-in-alpha: mapping `(R,A,0,1)`. It is suitable for
  any RG values; normal-specific quality statistics additionally assume positive Z.
- ASTC has no SNORM image format. The prototype remaps `s` to `(s+1)/2` during
  encoding and applies `2*u-1` before comparison. Vulkan component swizzles cannot
  implement that arithmetic. Correct general runtime support would require
  per-descriptor shader semantics for sampling/fetch/gather and constants, or
  keeping current R8/RG8 SNORM. The latter needs no new shader remap.
- HDR requires the enabled ASTC HDR feature and suitable SFLOAT format, separate
  from LDR/sRGB and UNORM8 decode mode. Unsupported/signed-negative cases keep
  RGBA16F until actually tested.

The existing private Dimensity Vulkan JSON reports no BC, ASTC LDR/HDR and
`VK_EXT_astc_decode_mode`; this is offline capability evidence only. Runtime
currently records LDR `V.has_astc`; extension presence alone does not enable
HDR/decode-mode features or prove correct views/copies. Query actual ASTC
sampled/filter/transfer capabilities and enable the required features before
use. Validation and correctness tests remain mandatory in a separately approved
step; this task has no rendering, load-time, memory-residency or performance
evidence on that phone.

The existing **NVN packaged-texture mechanism is not a distribution cache**.
`SetPackagedTextureLayout/Data` describes guest pool memory and its block-linear
uploads. Replacing BC bytes in RomFS with ASTC without changing the game's format
descriptors/layout would corrupt it. The porter's generic `datapack.build(files=...)`
can carry a sidecar in the external/embedded data distribution, but the runtime
has no existing source-hash-to-ASTC lookup. Add a versioned index/reader, owned
file cleanup, build-cache invalidation and miss/fallback policy in later work.
Preserve original guest data for CPU reads, dynamic writes and readback semantics.

The installation gate must eventually accept **native BC OR a tested ASTC path
with complete package/fallback coverage**, not ASTC feature presence alone.
Keep Adreno's native BC path. Leave `requires bcn` unchanged now: ASTC-only
quality fails, signed/view semantics and dynamic copy coverage are not validated,
and no device run has established a working alternative.

## License, checks and decision before step 2

Arm astcenc is Apache-2.0. A later source/library vendor must retain its license,
copyright/SPDX notices, identify modifications, and preserve applicable notices
and patent terms; the pinned tree has `LICENSE.txt` and no separate NOTICE.
Current use is a private tool/library, with no third-party code committed here.
[Pinned license](https://raw.githubusercontent.com/ARM-software/astc-encoder/d0f91386350222fcbf6da533c0f7a3f9b5c6b783/LICENSE.txt)
The repository's bcdec is used under its existing license. BNTX-Injector was
consulted for header-format facts only; its GPL code is not copied or linked.
[Format-reference structs](https://github.com/aboood40091/BNTX-Injector/blob/master/structs.py)

Checks: independent ASTC decode/block-info, full runtime layout comparison,
endpoint arithmetic/counterexample search and bank round trips all completed.
Full existing source suite: **165 passed, 0 skipped**, including the three
normally optional Yaz0 tests using the existing native decoder. No runtime
source changes; hashes match the base checkout. No Android builds are needed
for an offline documentation/prototype task.

**Recommendation for a separately approved step 2:** PC port-time transcoding
and a versioned sidecar; guarded fast BC1 conversion; retain R8 BC4 and SNORM
R8/RG8; use quality-gated ASTC for BC3/unsigned BC5, with existing lossless
decoded-format exceptions. Whole-image exceptions keep image/view semantics
tractable. Initially run even the fast converter on the PC to avoid assuming
phone upload-time safety. A sidecar has a substantial storage/port-time cost; a hybrid
direct-plus-fallback-patch package could reduce it but needs a new measured
design and cannot inherit the full-bank size estimate.

The user must decide whether to approve that mixed ASTC/lossless fallback design
and its storage/port-time costs, or require a different representation/research
direction. The unconditional eight-weight BC4 mapping and universal ASTC-only
38 dB target are not met. Do not remove the install gate or proceed to device
implementation based on this report alone.
