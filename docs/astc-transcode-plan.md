# BC to ASTC: offline evidence and complete sidecar implementation

The original step-1 analysis below is based on `b61cdce8`, branch
`perf/astc-transcode`. That analysis used no device commands,
game launches, APKs, runtime/default/profile changes, installation-gate changes,
production changes, merge or push. The supplied upstream investigation found no
transcoder to port; the optional repeat search was not needed for these prototypes.

The step-1b comparison appended below supersedes the initial storage recommendation.
It measures ETC2/EAC and a compact ASTC patch, including complete BC1 image quality.
The user subsequently chose the complete compressed mixed sidecar, not the patch
or ETC2-only alternatives. The step-2 implementation and PC-only validation are
recorded at the end. Phone validation and production changes remain separate.

**Result:** the guarded BC1/BC4 direct paths meet the one-byte error bound in the
reference-decoder sample. An unconditional eight-weight BC4 path cannot do so for
some real blocks. ASTC-only re-encoding fails the 38 dB requirement, even at
thorough: 46 unique payloads fail RGB or alpha, increasing to 50 when BC5's two
meaningful channels are checked without diluting their error with constant blue.
That historical result rejected universal ASTC re-encoding. The approved mixed
implementation retains lossless exceptions and the unchanged 38 dB gate.

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

## Step 1b: smaller alternatives, 2026-11-03

The user requested this offline comparison before choosing an implementation.
All tools, raw metrics, named exception lists and generated banks are private in
`BENCH/astc-transcode/alternatives/`. No device commands, Android builds,
porter/GUI/runtime changes or production changes. This section uses the same
owned game/DLC census and decoder/conversion sources as step 1.

### Option 1: ETC2/EAC capabilities and tools

The saved Dimensity JSON has `optimalTilingFeatures=119809` for **each** of
ETC2 RGB8, RGB8A1, RGBA8 (UNORM and sRGB), EAC R11 and RG11 (UNORM and SNORM):
all ten have sampled-image, linear-filter and transfer-source/destination support.
This is offline capability evidence, not a rendering or performance test.

Mapping: BC1 to RGB8 unless transparent selectors require RGB8A1; BC3 to RGBA8;
BC4 to R11 and BC5 to RG11 with signedness preserved. sRGB remains sRGB.
BNTX cannot identify the later BC1 RGB/RGBA binding, so punch-through content is
handled conservatively. EAC signed sampling naturally returns signed values;
it does not need the ASTC signed-value shader remap discussed above.

| Tool | Pinned revision | License | Configuration |
|---|---|---|---|
| etcpak 2.1 | `3d716e1550023dc5ff8b602636af8b76584e0b66` | BSD-3-Clause | Default ETC2 heuristics, one thread; RGB8/RGBA8/unsigned R11/RG11 |
| etc2comp | `39422c1aa2f4889d636db5790af1d0be6ff3a226` | Apache-2.0 | Effort 0 and high-quality effort 100, jobs=maxJobs=1 |
| detex reference decoder | `cab11584d9be140602a66fd9a88ef0b99f08a97a` | ISC | Independent ETC2/EAC decode |
| zstd | 1.5.7 | BSD license option | Level 3, independent image frames |

The cores are unchanged and their fetched blobs are hash-checked. etcpak's
[public encoder interface](https://github.com/wolfpld/etcpak/blob/3d716e1550023dc5ff8b602636af8b76584e0b66/ProcessRGB.hpp)
has no RGB8A1 or signed-EAC entry points: these are **unsupported**, not passes.
Future distribution must retain the respective licenses/notices; astcenc retains
its Apache-2.0 obligations. The tool/library sources and binaries are private here.
[etcpak license](https://github.com/wolfpld/etcpak/blob/3d716e1550023dc5ff8b602636af8b76584e0b66/LICENSE.txt),
[etc2comp license](https://github.com/google/etc2comp/blob/39422c1aa2f4889d636db5790af1d0be6ff3a226/LICENSE),
[detex license](https://github.com/hglm/detex/blob/cab11584d9be140602a66fd9a88ef0b99f08a97a/LICENSE).
The zstd BSD redistribution terms are in its
[versioned license](https://github.com/meta/zstd/blob/v1.5.7/LICENSE).

Metrics use the same bcdec reference and 38 dB RGB/alpha gate, with meaningful
R/RG checks so constant channels cannot hide error. EAC is decoded to replicated
16-bit normalized values; signed range has peak two. The adapter applies the
required signed-EAC base -128 to -127 rule, which detex itself omits. There are
694 such component-endpoint occurrences in each etc2comp signed run, including
atlas padding; no decoder errors. The
[Khronos signed-EAC rule](https://registry.khronos.org/DataFormat/specs/1.4/dataformat.1.4.html)
defines that corner case.

**Encoder-setting correction:** final BC3 uses etc2comp `RGBX`. Its `NUMERIC`
mode still uses alpha-dependent source averages and candidate heuristics, which
can discard RGB at zero alpha. That initial diagnostic is retained privately
and excluded from final BC3 scores. RGB8/A1/EAC retain their tested settings.
Opaque and varying-alpha known-bitstream tests verify channels/raster order;
64 real blocks' internal encoder reconstruction agrees with independent decode
within 6e-8 RGB and exactly in alpha. The remaining failures below are measured
tool/settings failures, not a proof that every possible ETC2 encoder must fail.
[Source-average metric handling](https://github.com/google/etc2comp/blob/39422c1aa2f4889d636db5790af1d0be6ff3a226/EtcLib/EtcCodec/EtcBlock4x4Encoding_ETC1.cpp).

Coverage: fast/effort-0 test **347 complete images / 271,475,772 valid texels**.
BC3 UNORM and signed BC4/BC5 include all unique payloads; other formats select
eight complete images from each size quartile, with the largest included.
Effort-100 calibration is slow, so its documented sample is **267 complete
images / 7,520,726 texels**: the same complete small-format sets, plus up to 16
images per other format, at most 262,144 texels each, retaining A1 coverage.
No resizing or partial-image PSNR. These sample failures are concrete
counterexamples, not full-census failure-rate estimates. Different preset
sample sizes must not be compared as if they were the same population.

Failures / tested images, checking RGB, alpha and meaningful R/RG:

| Source | etcpak | etc2comp effort 0 | etc2comp effort 100 |
|---|---:|---:|---:|
| BC1 UNORM | 29/29; 3 unsupported | 30/32 | 15/16 |
| BC1 sRGB | 13/28; 4 unsupported | 12/32 | 7/16 |
| BC3 UNORM | 153/154 | 152/154 | 149/154 |
| BC3 sRGB | 14/32 | 8/32 | 2/16 |
| BC4 UNORM | 2/32 in R; 0 in RGB | 0/32 | 0/16 |
| BC4 SNORM | 1 unsupported | 0/1 | 0/1 |
| BC5 UNORM | 0/32 | 0/32 | 0/16 |
| BC5 SNORM | 32 unsupported | 0/32 | 0/32 |

Representative per-image PSNR median / fifth percentile, in dB:

| Source | etcpak RGB; alpha | Effort-100 RGB; alpha |
|---|---|---|
| BC1 UNORM | 29.76/24.65; inf/inf | 32.50/25.35; inf/inf |
| BC1 sRGB | 38.08/31.47; inf/inf | 40.68/32.21; inf/inf |
| BC3 UNORM | 20.57/19.02; 63.31/45.92 | 27.86/26.29; inf/48.80 |
| BC3 sRGB | 38.66/31.49; 46.85/38.02 | 42.07/35.16; 50.70/43.73 |
| BC4 UNORM | 49.20/41.27; inf/inf | 57.96/50.92; inf/inf |
| BC4 SNORM | Unsupported | 54.34/54.34; inf/inf |
| BC5 UNORM | 47.75/43.02; inf/inf | 53.53/48.80; inf/inf |
| BC5 SNORM | Unsupported | 49.60/46.07; inf/inf |

Meaningful-R BC4 UNORM med/p5 is 44.43/36.49 dB fast and 53.19/46.15 HQ;
signed R is 49.57 dB HQ. BC5 UNORM RG med/p5 is 45.99/41.26 fast and
51.77/47.04 HQ; signed RG is 47.84/44.31 HQ. EAC therefore looks substantially
more promising than these ETC2 colour results, but no full-census EAC quality
acceptance or mixed ETC memory footprint has been established.

BC5 texel-weighted angular median/p95, using the same positive-Z reconstruction:

| Source | etcpak | Effort 0 | Effort 100 |
|---|---:|---:|---:|
| UNORM | 0.08/1.82 degrees | 0.04/1.23 | 0.05/1.00 |
| SNORM | Unsupported | 0.43/2.02 | 0.41/1.57 |

All-BC5 statistics are hypothetical normal interpretations. The normal-name
subset's median of per-image p95 angles is 1.76 degrees fast / 1.08 HQ for
unsigned data (8 / 3 sampled images), and 1.30 HQ for all 32 signed payloads.
Those per-image summaries are distinct from texel-weighted global p95. No new
angular threshold is invented. The approved mixed design retains signed RG8
losslessly, so these lossy signed-EAC angles do not improve on that baseline.

Serial PC encoder throughput, MPix/s, same i7-13700H as step 1, AVX2/FMA/BMI
optimized build, one native job. Three timed repeats after warm-up; fast calls
average 32 repetitions. These exclude BC decode, format preparation and IO.

| Source/target | etcpak | Effort 0 | Effort 100 |
|---|---:|---:|---:|
| BC1 UNORM / RGB8 | 326.868 | 0.672 | 0.013 |
| BC1 sRGB / RGB8 | 460.427 | 0.840 | 0.013 |
| BC1 UNORM / A1 | Unsupported | 1.616 | 0.020 |
| BC1 sRGB / A1 | Unsupported | 1.728 | 0.023 |
| BC3 UNORM / RGBA8 | 219.735 | 0.691 | 0.014 |
| BC3 sRGB / RGBA8 | 268.783 | 0.875 | 0.016 |
| BC4 UNORM / R11 | 285.727 | 1.321 | 0.020 |
| BC4 SNORM / signed R11 | Unsupported | 1.262 | 0.010 |
| BC5 UNORM / RG11 | 114.605 | 0.502 | 0.007 |
| BC5 SNORM / signed RG11 | Unsupported | 0.706 | 0.005 |

Planning assumption only: phone throughput at **10-25% of serial PC**, not a
measurement or guaranteed bound. Fast supported targets would encode a
1024-square image in roughly 9-92 ms, before decode/upload costs; this already
poses burst-upload hitch risk. Missing A1/signed targets need another encoder.
Effort 0 is roughly 2.4-21 seconds per 1024-square image, and effort 100 roughly
3-35 minutes. A complete synchronous on-upload encoder meeting this quality
criterion is **not established**. Fast supported encoding is computationally
plausible as background work, but its quality failures prevent recommending
it as the blanket replacement.

Content-dependent full-unique-set serial PC estimates: effort 0 about 3.20 h,
effort 100 about 8.57 days, excluding extraction, decode, gate checks and IO.
etcpak alone projects about 25 s for its supported population, but omits
372,247,536 texels in A1/signed targets; that is not a complete pack time.
A complete fast combination can use the already tested effort-0 results for
those targets: it fails **213/347** sampled images in RGB/alpha, or **215/347**
including meaningful R/RG. Its full-unique-set serial encode estimate is
243.7 s, before decode, gate checks and IO. This composed configuration is not
a new encode experiment and does not rescue the quality gate.
No full ETC pack was generated because the measured encoders fail the quality
gate. Its ideal logical payload would be 8,917,552,712 catalog bytes, or
5,235,060,320 unique bytes plus metadata. The same values describe ideal ETC
texture memory; lossless fallback for failures would increase that memory,
and its full-census extent remains unmeasured.

### Option 2: complete BC1 direct-plus-patch assembly

This test processes **every unique BC1 payload**, all layers/mips and valid
texels, then applies the image-level 38 dB RGB/alpha gate. It uses the unchanged
C converter and thorough ASTC fallback from step 1. Duplicate inventory entries
are accounted for exactly; this is no longer the 512-block sample.

| Source | Unique blocks / fallback | Full-census blocks / fallback |
|---|---:|---:|
| BC1 UNORM | 45,720,473 / 4,870,145 | 142,191,280 / 14,463,828 |
| BC1 sRGB | 476,396,315 / 122,799,739 | 741,334,243 / 154,831,069 |
| Total | 522,116,788 / 127,669,884 | 883,525,523 / 169,294,897 |

All fallback blocks are three-colour mode; no four-colour quantization rejects.
The **394,446,904 unique direct blocks** have maximum error **1/255**, with
zero violating channels. Full-image checks plus a separate test of all 117,765
edge blocks (78,510 accepted direct) include every padding texel as well.
No malformed ASTC blocks. The bound still requires the UNORM8 decode policy
described in step 1; fallback blocks themselves are not one-byte bounded.

| Assembled BC1 images | Tested / failed 38 dB | RGB med / p5 / minimum | Alpha minimum |
|---|---:|---:|---:|
| UNORM | 3,371 / **0** | 57.90 / 55.11 / **40.66** dB | 58.86 dB |
| sRGB | 7,964 / **0** | 58.74 / 55.02 / **41.13** dB | 46.01 dB |

The patch reuses complete, quality-checked thorough BC3/unsigned BC5 results.
It stores 347,424,128 bytes of passed images and **47 unique whole-image
exceptions**: 3 BC3 UNORM, 38 BC3 sRGB, 6 unsigned BC5 (73 catalog aliases).
BC4 and all SNORM remain on current lossless R8/RG8 paths. No new angular gate
was introduced. Included converted images meet their RGB/alpha/meaningful-RG
38 dB gate; exceptions are explicitly decoded losslessly, not relabelled passes
of the lossy ASTC encoder. Private `patch-exceptions.csv` contains every identity.
Unsigned BC5 retains the step-1 thorough result (pre-exception angular median/p95
0.00/1.04 degrees, normal-name subset0.00/1.62); replacing rejected images with
lossless decode does not increase their pixel error. Signed BC5 is lossless.

Patch bank, constructed and verified:

| Component | Bytes |
|---|---:|
| BC1 fallback ASTC blocks | 2,042,718,144 |
| Passed BC3/unsigned BC5 ASTC blocks | 347,424,128 |
| 13,713-entry index, 128 bytes/entry | 1,755,264 |
| Header | 128 |
| **Raw bank** | **2,391,897,664** |
| Independent zstd-3 image frames | 425,601,396 |
| Compressed alignment padding | 100,972 |
| **Compressed bank, including index/header/padding** | **427,457,760** |

There is **no per-block position table**. The pinned direct predicate determines
fallback positions in source-block order; full-source+geometry hashes and expected
fallback counts reject a mismatch. The diagnostic 65,267,760 bitmap bytes are
not stored. Source/target profile, mip/layer geometry and codec policy must be
part of a production format's version contract. This bank is a private prototype,
not an implemented runtime ABI.

Raw SHA256: `e9b409d77c89e3eb7e3ddc1d9e8b79d60960a379f834078af58709c9239ae5a4`.
Compressed SHA256: `7f1ce132d024360066b0441ba74163058e1ed0b69873a17f77e14b371ee44442`.
Every compressed image frame is independently addressable and round-tripped;
plaintext/index SHA256s and bank SHA256s are retained. The 427 MB size assumes
the phone reads/decompresses those frames. PC-side expansion before installation
would instead leave the 2.392 GB raw bank on the phone.

Fresh serial C timing, five repeats on one thread: direct UNORM/sRGB
**229.3 / 199.4 MPix/s**; direct plus ordered patch memcpy/count check
**229.8 / 221.5 MPix/s**. No affinity/frequency control; small differences between
these medians are timing variance, not an assembly speedup. Hashing, zstd,
deswizzling, IO and upload are excluded. At the same unverified phone scaling,
conversion alone is about **18-53 ms per 1024-square image**, before those costs.
The largest catalog BC1 image has 116,042,383 texels across its layers/mips:
roughly **2-6 seconds** of assumed serial conversion. Synchronous replay-time
conversion is therefore a risk; background/load preparation or a bounded cache
needs explicit design and device validation. Fully expanding a persistent cache
would recover much of the complete-sidecar storage cost.

Four-worker complete BC1 quality-run wall time was 572.877 s, overlapping the
ETC experiment. Raw+compressed bank build and verification took 287.077 s;
zstd compression plus immediate round-trip accounted for 11.911 s of that.
BC3/BC5 reuse step-1 outputs, so these are not timings for regenerating all inputs
from scratch. Serial throughput and phone assumptions are separate from these
shared-host wall times.

Logical texture memory, including the retained R8/RG8 paths and quality exceptions:
**16,874,636,483 catalog bytes / 10,123,802,415 unique bytes**. Converted BC1 is
14,136,408,368 catalog bytes, exactly twice its 7,068,204,184 BC source bytes.
This is identical for the complete mixed sidecar and patch representation.
It is a catalog footprint, not resident GPU memory, RSS or a device measurement.

### Comparison and decision

For a fair storage comparison, the **same approved complete mixed sidecar** was
also framed with the identical per-image zstd-3 policy. This does not change its
texture representation, quality gate or encoder candidate. BC1 is reconstructed
on the PC from the already verified direct+patch data; no new encoding search.
All image frames round-trip; a full uncompressed 8.7 GB file was not written.

- Complete mixed raw payload: 8,701,292,736 bytes; with the same index/header,
  **8,703,048,128 bytes raw** (derived exact size).
- Actual complete mixed compressed bank: **2,090,229,056 bytes**, including
  index/header and 101,772 alignment bytes. SHA256:
  `8652d965b5874a95c41894c8e14011189963898b662aec28a93eb6a7fd4d5790`.
- Build/hash/round-trip wall time: 206.841 s. Native BC1 assembly accounted for
  26.056 s; zstd compression plus immediate full round-trip for 38.927 s.
  These reuse the already encoded fallback/BC3/BC5 data.
- Patch saves **6,311,150,464 bytes (72.5%) raw**, or **1,662,771,296 bytes
  (79.5%) with the same compression**. Do not compare compressed patch against
  uncompressed complete sidecar and call that the compression-adjusted saving.

GB below is decimal. Added data is in addition to the original owned files.
Texture memory is the complete logical inventory, not simultaneous residency;
ETC memory assumes every image remains compressed and is therefore an ideal
lower-cost case that does not meet the measured quality gate.

| Representation | Added data/storage | Logical catalog texture memory | Phone work and hitch risk | Quality evidence | Complexity/licensing |
|---|---|---|---|---|---|
| Approved complete mixed ASTC sidecar | 8.703 GB raw; **2.090 GB actual zstd** | **16.875 GB**, includes lossless exceptions | Hash, IO, zstd and upload; **no BC1 conversion**. Decompression/IO still need a bounded loading path. | Complete BC1 passes; 47 BC3/unsigned BC5 lossless exceptions; BC4/SNORM lossless. | Content lookup before image creation, correct views/copies/re-uploads, versioned frames. astcenc Apache-2.0; zstd BSD option. |
| Option 1, ETC2/EAC encoded on phone | No texture sidecar | 8.918 GB ideal; enforcing lossless exceptions increases it, total unmeasured | Fast supported targets assumed9-92 ms per1024-square; A1/signed require another encoder. Complete higher-quality upload encoding can take seconds/minutes. | Measured colour encoders fail38dB; EAC sample promising. No universal acceptance. | New target formats, encoder, pre-creation quality/fallback selection. Fast BSD-3-Clause; HQ Apache-2.0. |
| Option 1, PC ETC2/EAC pack | 5.235 GB unique raw payload plus metadata, before exceptions; compressed full pack not generated | Same ideal8.918 GB, with unmeasured quality-fallback growth | No encode on phone; lookup/IO/upload. HQ full-set serial projection~8.57 days, not an actual full encode. | Same measured quality failures; a PC pack alone does not remove them. | Offline toolchain/gate plus runtime lookup/views. Same encoder licenses; no encoder needed on phone. |
| Option 2, ASTC patch | **2.392 GB actual raw; 0.427 GB actual zstd** | **16.875 GB**, identical to complete mixed sidecar | IO/hash/zstd plus direct BC1 conversion; assumed18-53 ms per1024-square, largest catalog image~2-6 s. Needs scheduling/cache policy. | All11,335 BC1 images pass38dB; direct blocks <=1/255; same47 lossless exceptions. | Same format/content issues plus pinned C converter, implicit patch-order/count contract and worker/cache logic. astcenc Apache-2.0 on PC, zstd BSD option, repository bcdec license. |

The patch prototype accesses whole images in canonical layer/mip order. Fine-grain
streaming may need per-subresource hashes/prefix counts or independently compressed
subresources; that metadata and runtime policy are not implemented or sized here.
Unknown contents, partial writes, missing/corrupt data and re-uploads must retain
the existing lossless path. Both ASTC alternatives still require resolving format
selection before physical image creation. Compression does not solve that issue.

**Recommendation:** start step 2 with the **compressed complete mixed sidecar**
if its measured 2.090 GB is acceptable. It preserves the user's original goal of
PC-made texture data and avoids introducing BC1 conversion on the phone. This is
a choice to reduce conversion-related hitch risk, not a claim of measured phone
performance; larger IO/decompression and memory still need device validation.
Choose option 2 if saving another 1.663 GB justifies the added conversion and
scheduling work. Do not choose blanket ETC2/EAC with these measured encoders
while retaining the 38 dB requirement; its attractive ideal memory size depends
on quality that the colour paths did not achieve. Further EAC-only research is
a separate decision, not an implemented third candidate.

The user must choose **2.090 GB complete compressed sidecar with no phone BC1
conversion**, or **0.427 GB patch with phone conversion and an explicit loading
budget**, before step 2 resumes. No quality threshold or runtime default changes
follow from this comparison. Raw data, corrected encoder-setting provenance,
codec/source hashes and verification results are indexed by
`BENCH/astc-transcode/alternatives/alternatives-manifest.json`.

Final offline checks: 90 codec smoke cases, known opaque/varying-alpha spatial
cases, internal/independent reconstruction, complete BC1/padding coverage and
all three 13,713-entry bank indexes pass. The unchanged repository source suite
was rerun: **165 passed, 0 skipped**, 107.681 s, native Yaz0 enabled. Only this
report and the perf-log entry are committed; no generated game data enters Git.

## Step 2: implemented complete compressed sidecar (2026-11-03)

The user selected the **complete mixed sidecar with per-image zstd frames**, not
the patch or ETC alternatives. This implementation is an offline build and host
verification result; Android rendering, memory and frame-time acceptance remain
for step 3. No device command, installation or production change was performed.
The earlier upstream search found no BC-to-ASTC implementation to port. Existing
archive readers, Tegra layout/deswizzle code and `bcdec` are reused unchanged.

### Generation and installation

Both `porter port --target-gpu …` and the GUI's Target GPU control select the
representation. `auto` uses the selected connected device's BC capability and,
for a non-BC device, positive Vulkan ASTC LDR/decode-mode evidence. Offline users
choose Adreno/Xclipse (native BC), or Mali/Immortalis/PowerVR (offline ASTC).
The small model map is TB323FU -> Adreno and V2405A/PD2405/Dimensity-9400 ->
Immortalis; unknown models require an explicit GPU family. An explicit target
does not contact adb. Generation of non-BC data currently requires a Windows or
Linux x64 PC; the existing native-BC path is unchanged on other desktop hosts.

`porter textures --romfs <owned RomFS> [--aoc <owned aoc>] --out textures.astc
--work <cache> --ndk <r27c> --jobs 4` also runs the final generator over an already
extracted game, without connecting a device. The regular data stage invokes the
same function. It scans BNTX images in loose files and nested SARC/Yaz0 archives,
uses native deswizzling, and canonicalizes **all layers, then all mips**, omitting
Tegra padding. Source content, geometry, policy, helper and encoder hashes key
the reusable output cache. At most four image jobs run; each external encoder is
single-threaded, with bounded 65,536-block atlases and replicated edge texels.

The policy is unchanged: guarded BC1 direct blocks must decode within one byte
of `bcdec`; the remaining BC1 blocks are PC-encoded. BC3 and unsigned BC5 use
`-thorough -decode_unorm8`; BC5 uses the encoder's normal-map RA representation.
RGB and alpha must each reach 38 dB over valid texels of the complete image;
BC5 additionally gates meaningful RG at 38 dB and records reconstructed-normal
median/p95 angular errors. No numerical angular threshold was approved, so none
is invented: non-finite angular results fail, and finite errors remain evidence
for step 3. Failed BC3/BC5 images become whole-image lossless exceptions. BC4,
SNORM and unimplemented source formats retain the existing decode path.

The official Arm astcenc **5.7.0**, commit
`baff485b0ff36d2f95d28961605106502c653966`, is fetched only for ASTC generation;
each supported release archive has a pinned SHA256. AVX2 is selected only after
CPU and OS checks, with SSE2 as the x64 baseline. The experimental encoder from
steps 1/1b is not silently reused: this final run regenerates and rechecks the
images with the pinned official tool. Apache-2.0 and the CLI's bundled third-party
notices are retained in `dm/third_party/astcenc` and copied with the tool and
desktop bundle. Android includes only the zstd **1.5.7** decoder/common sources
(upstream `f8745da6ff1ad1e7bab384bd1f9d742439278e99`, BSD-3-Clause option);
no ASTC encoder or BC-to-ASTC converter is linked into the phone runtime.
Generated APKs also retain the zstd BSD notice under `assets/nxnative/licenses/`.

Native-BC packs retain the original manifest and data. An ASTC pack adds
`textures.astc` and a `textures` record identifying `astc-mixed-zstd-v1`, the
whole-bank SHA256, bytes, entries, exceptions and policy. Its manifest substitutes
`astc-ldr` plus `astc-unorm8` for `bcn`, leaving the profile itself unchanged.
The installer requires positive ASTC LDR and `VK_EXT_astc_decode_mode` support,
and checks the bank hash and every image frame on the PC. A native-BC pack still
requires `bcn`. Switching data packs removes a stale sidecar as managed content.
The original owned RomFS and BC bytes remain in the pack; the sidecar is additive.

### Wire format and runtime image decision

Version 1 has a 128-byte `NXASTC01` header, sorted 128-byte index entries and one
independent zstd-3 frame per accepted whole image. The index key is
`SHA256(SHA256(tight BC bytes) || LE32(format,width,height,mips,layers,1))`.
An entry records source format/geometry/block count, source and ASTC byte counts,
frame offset/length, plaintext SHA256 and an exception flag. Header/index SHA256,
policy pin, exact geometry/lengths, sorted unique keys and contiguous aligned
ranges are checked before use; decompression rejects multiple/trailing frames,
wrong content size and plaintext checksum mismatches. Limits are 262,144 entries
and **256 MiB decoded ASTC per image**, checked before allocation. The writer
round-trips every frame, verifies the completed bank and atomically replaces its
output. There are no generated timestamps in the bank. Byte identity is scoped
to identical pinned tool/ISA, generator and compression-library inputs.

Before physical `vkCreateImage`, `astc_fetch()` in `gfx_vk_tex.c` deswizzles and
hashes the published guest BC contents and looks up the complete image. A verified
hit chooses ASTC 4x4 UNORM/sRGB; an exception, missing/unknown content, unsupported
target/usage or missing/invalid bank keeps the existing lossless decode. Eligible
targets are non-sparse, non-linear, single-sampled 2D/array/cube images with
known pool storage, outside image-store use. An image first encountered without
published contents or outside a submission remains on the legacy path. On a
native-BC device the guard returns before opening or hashing the bank.

ASTC upload uses the same 4x4 block grid, including small edge mips and every
layer. BC1 RGB views force alpha one; BC5 exposes `(R,A,0,1)`; guest/view swizzles
compose with those mappings. UNORM views request 8-bit ASTC decode so the PC
one-byte check matches the intended hardware decode; sRGB uses the ASTC sRGB
format. The shared-exponent feature is not needed for UNORM8 decode.
See the [Khronos decode-mode definition](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewASTCDecodeModeEXT.html).

An ASTC image retains its original tight BC snapshot while live. A known CPU
re-upload fetches its matching frame. Unknown CPU contents replace storage with
the legacy decoded image. An arbitrary GPU copy/write, attachment use or
incompatible format view first restores **all original BC subresources** into
lossless storage, preserving untouched regions before the write. Replacement
lands/splits the submission, defers old handles, invalidates cached views and
advances the image epoch while retaining the logical image pointer. This avoids
changing an image's Vulkan format in place or reconstructing lossy ASTC texels.
The snapshot adds host memory beyond the logical texture inventory; its live
bytes and demotions are reported and require phone measurement.

Only the small index is read at first use. Frames decompress on demand; the
whole pack is never expanded on replay. This version performs per-image
deswizzle/hash/lookup and decompression synchronously. The 256 MiB limit bounds
one decoded allocation, **not** total process or staging memory and **not** a
frame-time guarantee. The host benchmark below measures the actual production
`astc_fetch()` and decoder across the generated bank. Its decompress/hash log
counter excludes the preceding BC deswizzle/source hash; the separate full-fetch
benchmark includes them. No phone performance is inferred from the PC results.

Every 300 presents, the runtime logs `ASTC sidecar: lookups=… hits=… misses=…
exceptions=… failures=… demotions=…; … MB uploaded, … MB BC shadow;
decompress+hash … ms total, … ms max (cumulative)`. A bad index or image frame
reports a RED diagnostic and falls back to BC decode. A valid exception is
ordinary fallback, not a RED. Pacing, pair warming, shader code, prediction and
the PPE2 ABI are unchanged.

### Tests, generated artifacts and remaining validation

The final source suite passes **181 tests, 0 skipped** (180.465 s). New cases
cover offline/automatic target selection without real adb, CLI and native Qt
GUI forwarding, capability/manifest gates, deterministic banks and corrupt
headers/indexes/frames, guarded direct BC1 blocks with the Arm reference decoder,
encoder fallback and whole-image quality exceptions. Host C tests compile the
production bank reader and extract the actual runtime image functions, with
mock Vulkan calls: all mip/layer upload bytes, native BC, ASTC, exceptions/misses,
known and unknown CPU re-uploads, failed replacement, view swizzles and preservation
of untouched texels during partial GPU writes are checked. Release source checks
and real debug/release compilation verify the sidecar path is present in both.
All tests in this stage are offline; Vulkan mocks are not validation-layer proof.

Debug, validation and release libraries compile all **157 Android sources with
0 failures**; validation packages the same debug library plus the Khronos layer.
Existing APKs are retained. The new APKs have no embedded game assets; the large
external data pack is a separate private output. `BENCH/astc-sidecar-build.json`
records commands, source/library/APK hashes and protected older APK identities.
The native Windows Qt four-tab render was inspected; helper sources, NumPy and
licenses are wired into bundle packaging, but a frozen Windows bundle was not
built or published in this step.

### Complete generation result

The final tool scans all **48,157 files / 16,519,555,942 source bytes**, finding
32,250 eligible catalog images / **13,713 unique entries**. A fresh image-cache
run takes **3,302.485 s (55.04 min)**, including the scan, encoder/reference
decoder, image gates, compression and verification; the NDK and official encoder
were already cached. This exceeds the earlier approximately 27-minute encoding
projection. Verified image outputs are reusable, but subsequent ports still scan
the inputs and write/verify the bank.

| Source | Unique images | Lossless exceptions | Retained RGB min / p5 / median, dB | Retained alpha min, dB |
|---|---:|---:|---:|---:|
| BC1 UNORM | 3,371 | 0 | 40.659 / 55.107 / 57.901 | 58.856 |
| BC1 sRGB | 7,964 | 0 | 41.132 / 55.023 / 58.738 | 46.011 |
| BC3 UNORM | 154 | 3 | 45.947 / 48.359 / 51.535 | 38.709 |
| BC3 sRGB | 621 | 38 | 40.478 / 43.544 / 53.976 | 38.007 |
| BC5 UNORM | 1,603 | 6 | 40.487 / 44.477 / 54.586 | exact |

All retained images meet the unchanged 38 dB gates. BC5 meaningful RG min / p5 /
median is **38.726 / 42.716 / 52.825 dB**. Among retained BC5 images, the median
of per-image median angular errors is 0 degrees; median / p95 / maximum of their
per-image p95 errors is **0.461 / 2.866 / 9.202 degrees**. These unweighted image
statistics are not a new angular acceptance threshold. Normal-map appearance,
including the tail, needs phone review. The **47 unique exceptions / 73 catalog
aliases** are listed only in private `product/exceptions.csv` and
`product/cache/generation.json`, with quality metrics and reasons.

All **522,116,788 unique BC1 blocks** are covered: 394,446,904 guarded direct
blocks have maximum reference-decoded error **1/255**; 127,669,884 blocks
(24.452%) use PC encoder fallback. Every BC1 image passes its image gate.
BC4/SNORM continue to use lossless decoding, with no ASTC blocks stored for them.

| Complete sidecar component | Bytes |
|---|---:|
| Accepted full ASTC image bytes, before compression | 8,701,292,736 |
| Header and 13,713-entry index | 1,755,392 |
| Independent zstd image frames | 2,088,371,892 |
| Frame alignment padding | 101,772 |
| **Installed sidecar** | **2,090,229,056** |

Sidecar SHA256:
`a98538dce504ac769f0a4450734d46b3342216f5b7c3d85a93b352e7b40d1012`.
Its compressed size matches the earlier experiment; its production wire
contract/index differs, so the earlier experimental bank is not interchangeable.
There are 13,666 complete compressed image frames plus 47 exception records.
The PC's reusable raw BC/ASTC image cache is about **13.23 GB**, in addition to
the sidecar and metadata; it is not included in the phone data pack.

Logical inventory remains **16,874,636,483 catalog bytes / 10,123,802,415 unique
bytes**, versus 61,495,964,017 catalog bytes through the current CPU decode.
These totals exclude GPU allocation padding, staging, live-set selection and
retained BC snapshots; they are not process RSS or a measured phone saving.
The largest accepted whole-image ASTC payload is **116,044,624 bytes**.

The final staged external pack is private
`BENCH/astc-transcode/product/ZeldaBreathOfTheWild-astc-data`: **48,182 files /
19,232,312,322 bytes**, including its control files. Staging and the complete
source/frame/file audit take another **392.811 s**. All 48,157 owned source files
match their source hashes, with zero mismatches; the original data manifest,
preferences and settings remain unchanged. The new pack's environment/settings
bytes also match the originals. No primary save is included.

The data manifest SHA256 is
`50534b643dc658b5e5ae41df6143fccde8109d50489cc7837b69de5d8757d02c`;
the private complete `product/pack-files.json` file inventory has SHA256
`bd0238615fb541a082e6c6dafc5af26affe3a7877a2994faf7e5fd9aef852527`.
Rewriting the entire bank from the verified encoded-image cache takes 448.551 s
and produces the identical 2,090,229,056-byte bank and whole-file SHA256. This
checks deterministic assembly/compression with the same pinned inputs; it is
not a second full scan/encode timing. Step 3 must select the desired APK with
the installer's explicit `--apk` argument for this privately named data pack.

### Production-code host latency and repeat checks

Two serial C benchmarks verify **all 13,713 entries**, including the 47 ordinary
exceptions, with zero lookup mismatches or frame verification failures. They
compile the production bank reader, SHA implementation and zstd decoder at
`-O2`; the full-fetch benchmark also extracts the actual `astc_fetch()` and uses
the runtime Tegra layout/deswizzle code with prepared guest pool memory. It
performs no Vulkan upload. Timings below cover the **13,666 accepted images**,
one pass in sorted index order, on the i7-13700H Windows host:

| Operation, milliseconds per image | Median | p95 | p99 | Maximum |
|---|---:|---:|---:|---:|
| Complete `astc_fetch()` | 1.147 | 26.419 | 119.445 | **1,516.219** |
| Its frame IO + zstd + plaintext SHA | 0.759 | 18.075 | 82.158 | 1,058.388 |
| Remaining deswizzle + source SHA + lookup/allocation | 0.381 | 9.685 | 40.115 | 457.830 |
| Separate reader's index lookup | 0.0036 | 0.0072 | 0.0094 | 0.1606 |

The first three rows are measured within the same full-fetch pass; percentiles
are independent and cannot be added. The largest 116,044,624-byte accepted image
is the 1.516 s case. This synchronous path **can hitch**; bounded allocation and
per-image frames do not establish a useful frame-time bound. The measurements
retain this risk for review rather than treating the median as an acceptance.
The built-in runtime log reports only the second row, so it must be considered
alongside frame/load timing when checking the phone.

The generator and other benchmarks were stopped during each measurement pass.
There was no affinity/frequency control; the bank had recently been verified,
but OS cache residency was not controlled. Full-fetch timing excludes loading
the BC source file and the harness's preparation/swizzle of guest memory;
that guest source memory is warm. Cold IO, realistic demand order, actual Vulkan
uploads, Android CPU cost and phone frame times are unmeasured. There is no
PC-to-phone throughput extrapolation or claim that large textures fit a frame.

Six additional **uncached** codec repeats cover all five stored source formats
(including BC1 encoder fallbacks) and a quality exception. Encoded SHA256s,
direct/fallback decisions and quality/exception results match the original run.
The complete bank repeat checks assembly/compression separately as above.
Raw CSVs, benchmark sources/build scripts, complete file/source hashes, quality
exceptions and repeat records remain under `BENCH/astc-transcode/product/`.
`BENCH/astc-sidecar-validation.json` indexes these results; the corresponding
`astc-sidecar-build.json` records source-to-library-to-APK identities.

The final APKs include the decoder license. Earlier same-library APKs without
that asset are retained under their initial names; use these **`*2.apk`** files
for step 3:

| APK under BENCH | Bytes | SHA256 |
|---|---:|---|
| `astc-sidecar-debug2.apk` | 9,754,463 | `ffaaf1ba832f5b1ba1957067119e54ee18899275d9074026a3ea438e9d6be60d` |
| `astc-sidecar-validation2.apk` | 37,443,707 | `97bbf5fb34e16bf19c11faa020264424ccb5a6bca476964f404413c3ef1c0309` |
| `astc-sidecar-release2.apk` | 3,209,135 | `0e9d049eaee0d866a46231c9c0c8d6aca5569a772b11679894c2a72bcd443b77` |

Step 3 must validate the real phone install/capability gate, normal rendering
(especially sRGB, alpha, BC5 normals, mips, cube/array textures), save/load and
movement with the Vulkan layer; confirm meaningful sidecar hits and expected
exception/BC4/SNORM fallback with no new RED; and measure load time, large-image
fetch/upload stalls, re-upload/demotion correctness, live BC-shadow/RSS/PSS and
frame-time tails. The saved phone JSON is evidence of capabilities, not an
installation or rendering test. The existing lossless path's raw compressed
readback/reinterpretation limitations are not solved by this sidecar. No device
acceptance or production promotion follows from these host results.

## Step 3: Dimensity 9400 phone validation, 2026-11-03 — BLOCKED

The user approved BotW 1.9.0 / v1114112 + DLC1/2 on vivo V2405A
(`10AEBS0BKY001KZ`, MT6991 / Immortalis-G925), handheld, stock internal DRS,
target60, `DM_FPS_HOLD=off`, audio on and unchanged balanced device settings.
The protocol specifies an isolated owned Plateau seed, Vulkan/visual checks,
save/reload plus30s smoke, a60s warmup/120s moving route,600s continuous run,
and the same-debug-APK OFF/ON comparison. OFF disables both pair warming and
pacing budget; ON uses defaults. Limits remain cadence OFF+0.5pp, >50ms count
no higher than OFF, startup OFF+10s, RSS ON-OFF <=250,000,000 bytes, and no app LMK.
The private protocol and all result rows are in `BENCH/astc-phone-validation.json`.

**Installation reached92%; no game validation ran.** The positive ASTC LDR and
UNORM8 capability gate and complete host bank verification passed through the
normal `python -m porter install <generated ASTC pack> --apk ...release2.apk`
player path. The release APK installed; its on-phone SHA256 matches
`0e9d049eaee0d866a46231c9c0c8d6aca5569a772b11679894c2a72bcd443b77`.
A vivo `PackageInterceptActivity` was briefly observed and then resolved without
agent UI confirmation. No system-setting or account-verification change was
required. Two initial host dependency attempts failed before installation;
the existing `tools/perf-python` backports.zstd dependency resolved them.

After three automatically recovered adb push failures, the read-only keyguard
watcher's `dumpsys window policy` query timed out after20s at about15:13:25.
Its stop marker caused the controller to terminate porter at15:13:27. Last
progress was16.4/17.9GiB (92%), `textures.astc` piece7/32,29m44s into the successful
installation attempt. This was a controller/adb monitoring stop; no evidence
establishes a relock or game failure. The last watcher observation was unlocked.
All watcher/installer/validation-waiter processes ended. No retry, debug install,
seed preparation or device experiment followed this blocker.

| Approved criterion | Phone evidence / result |
|---|---|
| Zero validation errors; baseline warnings | Not run; errors and warnings unknown |
| Title/load/Plateau/route-end, including normal maps | Not run; no game screenshots or comparison |
| Sidecar hits/misses/exceptions, phone fetch distribution, long-frame overlap | Not measured; no runtime counters or phone benchmark execution |
| Isolated save/reload +30s gameplay, stable PID, RED0 | Not run; seed not copied |
| 120s moving route after60s warmup | Not run; fps, >17.5ms%, p99, >50/>100, max unknown |
| 600s continuous run, RSS/PSS/BC shadows, thermal/caps, no LMK | Not run; no gameplay time series |
| Debug OFF versus default ON | Neither run; all four limits and app-LMK criterion unevaluated |

The fresh Vulkan report confirms no BC, ASTC LDR/decode-mode support and no
descriptor buffer. `VK_GOOGLE_display_timing` is advertised; runtime operation
and pair-warm counters remain unmeasured. One pre-test HAL baseline reports
CPU/GPU39.318C, skin33.593C, battery31.4C, severity0. CPU `scaling_max_freq` reads
were permission-denied; no cap or game-memory result is inferred. Game Mode
reports `standard`; device performance settings, rotation and30s screen timeout
were not changed. The tablet was not accessed. Full local source checks pass
**181/181,0 skipped** in171.141s; the Android production-fetch CPU benchmark was
compiled but never pushed or executed. These are host checks, not device acceptance.

**End state is safe but incomplete:** exact release2 remains installed, app
stopped, screen OFF, no debug/validation APK installed, temporary UI XML removed.
There are17,813,171,626 retained external bytes, including10/32 resumable64MiB
sidecar pieces (671,088,640 bytes). The complete `textures.astc` and final
`nx2a-data.json` are absent, so the installation cannot start the game. The
requested complete-data/seed end state is not met. Partial files remain for a
normal porter resume; roughly1.42GB of the final pack remains. Original PC packs,
APKs and the production registry/export are unchanged.

Raw evidence, setup screenshot index, capability report, source-suite logs,
partial-file inventory and final package/power checks are under
`BENCH/astc-phone-validation/session-20261103-step3/`. All acceptance cells remain
**not evaluated**, and publication is blocked pending a new supervised session
that completes installation and the approved protocol. No merge, push, tag,
version bump or pre-release publication was performed.

## Step 3 continued: transfer verified; startup RED gate, 2026-11-03

The supervisor explicitly resumed the approved protocol after the transient
transport stop above. Original artifacts, device, balanced settings, handheld
stock DRS/target60/HOLD off/audio on, and acceptance thresholds are unchanged.
Installation/file-transfer reads now allow120s, backoff/reconnection to the same
serial for up to600s, and at most10 consecutive operation failures; harmless
wake events run about every10s. A measurement disconnect may invalidate/restart
its run once; no measurement disconnect occurred in this continuation.

**The normal porter installation and complete integrity audit now pass.**
The retained17,813,171,626 bytes were kept. All10 existing64MiB sidecar pieces
passed SHA256 and were reused;22 pieces were added. All32 pieces were verified
before assembly and again from the assembled file. The final bank SHA remains
`a98538dce504ac769f0a4450734d46b3342216f5b7c3d85a93b352e7b40d1012`.
On-phone SHA256 checks match **all48,182 pack files,0 mismatches**, including
the final manifest (`50534b64...`). The release APK on-phone hash matches
`0e9d049e...`. Resume plus audit takes446.063s; two adb failures during the
read-only file audit recover under the approved tolerance. No10-failure or
600s-absence limit is reached. Raw retry/reconnect records retain their byte
observations; a separate normalized log attaches the latest committed transfer
total to observations from the independent keepawake process.

**V starts with the exact validation APK97bbf5fb, then the strict RED gate stops
it before the title detector.** The layer is on. The retained log contains
0 Vulkan validation errors and0 validation warnings up to that point, plus
this one nonvalidation runtime line:

```text
[RED] memory pool <address> +0x123000 could not be zero-copy (-1): a GPU copy is refreshed before each submission (slow)
```

This comes from `gfx_vk_core.c:397`, `pool_add_one`, which selects the existing
copied-pool fallback. It is present in the unchanged release path too. The
zero-copy rejection's cause and the fallback's frame-time/memory cost are not
diagnosed. It does not establish a crash or Vulkan validation failure. The
controller's health check deliberately stops the process on this raw RED;
the smoke requires RED0, and no exception for this nonvalidation line was
applied. No debug/performance run, logging suppression, runtime fix, setting
change or additional game experiment follows this stop.

| Approved item | Continuation evidence / status |
|---|---|
| Complete non-BC install | PASS:48,182 file hashes;32/32 sidecar chunks; release/bank/manifest identities match |
| Vulkan validation | PARTIAL: layer on,0 errors/0 validation warnings observed;1 runtime RED; V not completed |
| Title/load/Plateau/route-end, BC5 normals | Not reached; no phone game screenshots or side-by-side comparison |
| Sidecar coverage/fetch distribution/long-frame overlap | Not measured; no600-present sidecar counters, native benchmark not pushed/executed |
| Save/reload +30s, RED0 and stable PID | Not run; startup raw RED1 exceeds0; final seed integrity is not a gameplay smoke |
| 120s moving route +60s warmup,600s continuous run | Not run; fps, >17.5ms%, p99, >50/>100 and max remain unknown |
| Same-debug OFF/ON comparison | Neither run; all regression limits remain unevaluated |

There is one startup sample for PID31586: RSS1,481,208KiB
(1,516,756,992 bytes), PSS1,326,189KiB (1,358,017,536 bytes).
Current HAL CPU/GPU73.007C, skin35.819C, battery33.6C, severity0;
CPU-policy maximums0/4/7 are2.1/3.0/2.7GHz. These are observations, not an
attribution of caps or a measured gameplay series. Standard devfreq exposes no
GPU cap sample. BC-shadow residency remains unknown and must not be confused
with the copied memory pools. Game Mode remains `standard`. Pair warming logs
enabled/four workers, but no completed counters; Google display timing remains
advertised with runtime presentation samples unmeasured.

**The requested complete end state is now met:** release2 hash0e9d049e,
complete pack/bank and manifest retained, ordinary save restored from the owned
seed copy with all17 file hashes matching. Validation/debug APKs and all private
test roots/temporary UI, trace and probe files are removed. App stopped,
screen OFF,30s timeout and system performance settings unchanged. Final bank
and manifest hashes are checked again. The tablet, PC production export and
production registry remain untouched.

`BENCH/astc-phone-validation.json` appends this continuation and preserves the
initial blocker snapshot/commit6d009761; new raw evidence is under
`BENCH/astc-phone-validation/session-20261103-step3-resume/`.24 runtime/porter
source hashes and all three APK hashes still match the build. The prior
181/181 source checks with0 skips are retained, not rerun for these records.
Publication remains blocked by the raw RED gate and incomplete device
acceptance. Further testing needs supervisor review of that baseline line or
a separately approved compatibility investigation; no criterion is relaxed.
No merge, push, tag, version bump or publication is performed.

## Step 3 baseline classification and V failure, 2026-11-03

The supervisor explicitly classifies the one `pool_add_one` zero-copy(-1)
message as a device-capability baseline notice: AHardwareBuffer import fails
on this vivo/Mali device and the pre-existing runtime selects its copied-pool
fallback. It is independent of ASTC and is not a correctness failure. The
private health check now permits **exactly one matching line per process**;
any other RED or repetition still fails. Raw log counts are preserved, with
approved baseline and unexpected RED counts reported separately. This approval
supersedes the prior strict-startup-RED blocker without changing APKs or code.

V restarts with the exact validation2 APK97bbf5fb, the owned isolated seed,
handheld/stock DRS/target60/HOLD off/audio on, and unchanged balanced settings.
The plain vivo risk acknowledgement/Continue installation is confirmed through
its observed UI; no setting/account change is required. The baseline line is
accepted once and does not recur. Existing `DM_VK_PROF_SLOW=1` is enabled in V
only to observe pool-sync cost; no measured APK is modified. OFF/ON profiling
would remain off, but those runs are not reached.

**V fails with one genuine Vulkan validation error**, after the baseline line:

```text
VUID-RuntimeSpirv-NonWritable-06341
vkCreateGraphicsPipelines(): vertex shader descriptor Set0/Binding2,
variable "vp_s0", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER lacks NonWritable,
but vertexPipelineStoresAndAtomics was not enabled.
```

The layer is on. The retained log has **raw RED2 = approved baseline1 +
unexpected validation error1**,0 validation warnings. Required validation
errors0, observed1: criterion1 FAILS by1. The process is deliberately stopped
by the harness; a game crash or memory kill is not established. No shader or
feature fix, APK rebuild, threshold change, extra run or experiment follows.

### Recorded fallback cost, limited to startup

- Notice pool: address`0x7a2300a000`, size`0x123000` = **1,191,936B /1.13671875MiB**.
- The first24 allocation logs show zero-copy0.0MiB and cumulative copied
  allocation162.6MiB. These capped startup allocation messages are not live
  residency or bytes copied per submission.
- No600-submission `copied pools` window is reached, so MiB/submission remains
  **unmeasured**. The existing incremental sync may copy only dirty64KiB blocks;
  the notice pool's full size must not be multiplied by submission count.
- Existing slow-replay diagnostics for presents0/1 report75.8/216.0ms total
  replay and0.0/0.0ms pool-sync, rounded to0.1ms. These two startup samples do
  not quantify steady copying cost. Pool-sync timing includes tracking and
  locks and is aggregate across copied pools, not isolated memcpy for one pool.

Startup PID23418 has RSS1,434,524KiB (1,468,952,576B) and PSS1,287,677KiB
(1,318,581,248B). Current HAL CPU/GPU69.614C, skin34.848C, battery33.3C,
severity0; CPU policy0/4/7 maximums2.1/2.7/2.9GHz, Game Mode`standard`.
No GPU devfreq cap sample is exposed. These are one startup observation,
not a gameplay memory/thermal series or attribution of caps. BC-shadow
residency remains unknown. Initial1280x720 target/crop and2800x1260 swapchain
are logged, but main-pass DRS sampling and title detection are not reached.
One display-timing refresh16.605ms is logged; actual presentation feedback is
unmeasured. Pair warming announces four workers without completed counters.

| Remaining approved item | Result |
|---|---|
| V validation | FAIL:1 error versus required0; baseline notice separately accepted once |
| Title/load/Plateau/end visual and BC5 normal checks | Not reached |
| Sidecar coverage, phone fetch distribution, long-frame correlation | Not run; no sidecar counters; native probe never pushed/executed |
| Isolated save/reload +30s gameplay | Not run after V failure |
| 120s route/60s warmup and600s continuous run, all frame/memory tails | Not run |
| Same-debug OFF/ON no-regression | Neither run; all thresholds unevaluated |

Required safe end state is restored again: exact release2/full data, final
bank SHA a98538dc and manifest50534b64 verified, all17 owned-seed file hashes
matched before atomic rename into the ordinary save while stopped. All
validation/debug builds and private test roots/probe/trace/UI files are removed;
app stopped, screen OFF,30s timeout and device performance settings unchanged.
The tablet and PC production registry/export remain untouched.24 runtime/porter
source hashes and all three APK hashes still match; the prior181/181 source
checks with0 skips remain applicable and are not rerun for record-only edits.

`BENCH/astc-phone-validation.json` appends this attempt and preserves both
earlier blockers. Evidence and exact baseline matcher/copy-cost limits are in
`BENCH/astc-phone-validation/session-20261103-step3-baseline/`, including
`baseline-classification.json`, `V/failure.log`, raw profiler/memory/thermal
data, setup UI screenshot and final state proof. Publication remains blocked
by VUID06341 and the unexecuted acceptance tests. A separately scoped
correctness investigation/fix and new validated APK are needed before testing
can continue. No merge, push, tag, version bump or publication is performed.

## Step3 continuation: Mali NonWritable correctness fix (2026-11-03)

The supervisor authorizes this general Vulkan compatibility fix and a fresh V
run with APK3. Earlier failed attempts above remain historical evidence. The
complete generated pack and sidecar are reused without regeneration or transfer.

`gfx_vk_core.c` copies the queried core features into the enabled feature set,
disabling robustness and Float64. It therefore enables
`vertexPipelineStoresAndAtomics` only when available. That enabled value is now
recorded explicitly. The translator's `DEC_NONWRITABLE` was unused. Targeted
searches after fetching nx2android34333894 and nxnative d145b956 found no related
fix to port. Khronos requires read-only storage declarations when the feature
is absent; the [SPIR-V decoration specification](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html)
permits NonWritable on the storage-buffer variable, for both StorageBuffer/Block
and Uniform/BufferBlock representations. See also the
[core feature definition](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html).

The runtime now inserts variable NonWritable annotations before types only for
vertex, geometry and tessellation SSBO bindings proven unwritten by the existing
instruction analysis. Unknown or unreflected bindings receive no assertion.
Real storage writes remain intact and produce one RED naming the shader. The
retained base and `shader_module` variants follow this rule; alternate debug
SPIR-V is analysed independently. Fragment, compute and devices enabling the
feature retain their original modules.

Offline census of the unchanged owned bundle (SHA716c5c60):

| Item | Count |
|---|---:|
| All shaders | 19,592 |
| Vertex / fragment / compute | 8,931 / 10,560 / 101 |
| Vertex SSBO shaders requiring NonWritable | 2 |
| Variable annotations inserted | 2 |
| Actual SSBO-writing shaders in affected stages | 0 |
| Unresolved / malformed / reflection overflow / patch failure | 0 / 0 / 0 / 0 |
| Feature-enabled modules byte-identical | 19,592 / 19,592 |

Native tests execute the actual base, variant and alternate module paths with
mock Vulkan creation, verify executable-byte preservation, unwritten-only
annotations, feature/stage gates, per-shader RED de-duplication and release
compilation. Independent spirv-val validates generated Vulkan1.0 BufferBlock
and1.1 StorageBuffer cases. Full source suite: **182 PASS,0 skipped,193.009s**.
Debug and release each compile157 sources with0 failures.

| APK3 | SHA-256 |
|---|---|
| debug | `3f02bf73a09ce4d291512ffe1d9c486648951bb1f5c783e1ad5bfc0f21bd42cb` |
| validation | `d98ee06004fa759d82f7edc6e42b1ad68448b8cb3f16c2a80f4d92678eac2f03` |
| release | `e85e77ce04aedb012b5e536fec30ed50542d0694e6bea56e66213aebb41234c1` |

Private build commands/library/input hashes and licenses are recorded in
`BENCH/astc-sidecar-build3.json`; the production notices remain packaged.
The census and this continuation's evidence live in
`BENCH/astc-phone-validation/session-20261103-step3-nonwritable/`.
Host correctness checks alone do not establish phone acceptance; installed-data
hash audit and the restarted V/remaining protocol are recorded below when run.

### APK3 installed-data audit and restarted V outcome

The same phone is online at transport13. The supervisor unlock is observed
read-only, with harmless wake events every10s while stopped. The existing
**19,232,312,322B** pack is reused: **48,182/48,182 file SHA-256s match,
0 mismatches**,109.75s; **32/32 installed sidecar chunks** are independently
reverified against the original bank. **No bulk data is resent**, no transfer
retry/reconnect or measurement disconnect occurs. Validation3 and final release3
install successfully, with installed APK hashes matching the table above.
An installer helper observes a window-list entry followed by a launcher/app
transition, refuses all taps and exits; no settings/account action is requested
or performed. The actual successful installation/hash checks remain decisive.

Fresh V uses the original isolated seed and settings, with the layer enabled.
It advances through startup to the last logged slow-replay present108, with
**0 Vulkan validation errors,0 validation warnings and no VUID06341 recurrence**.
However, it stops before title detection on a new unapproved RED:

```text
audio #0: the device has not called back for 201 ms while playing:
the silent sink stands in, retrying the AudioUnit
```

Raw RED2 consists of exactly one approved zero-copy baseline and this audio
RED. Logcat records dead IAudioTrack and AAudio error-899
`AAUDIO_ERROR_DISCONNECTED`; a GPU-completion timeout is also retained.
These observations do not establish root cause or an ASTC relationship.
No audio exemption, workaround, relaunch or further experiment is inferred.
PID5986's exit record is USER REQUESTED/FORCE STOP, confirming the harness
stop rather than an app crash or low-memory kill.

| Required criterion | APK3 result |
|---|---|
| Complete V, zero validation errors | Incomplete: observed errors0/warnings0 before audio gate |
| No unapproved RED (baseline exempt once) | FAIL:audio1 versus required0; baseline1, no recurrence |
| Title/load/Plateau/end visuals, BC5 normals | Not reached; no game reference comparison claimed |
| Sidecar coverage/fetch distribution/long-frame coincidence | Not run; bank opens, counters not yet printed; phone probe never pushed |
| Isolated save/reload plus30s smoke | Not run; a startup save commit is not this criterion |
| 120s moving route/60s warmup and600s continuous run | Not run; fps/cadence/p99/>50/>100/max and memory series unknown |
| Same-debug OFF/ON | Neither run; all five no-regression thresholds unevaluated |

Only startup memory is sampled: RSS1,431,004KiB (**1397.465MiB**),
PSS1,253,797KiB (**1224.411MiB**). Current HAL CPU/GPU74.751C,
skin35.687C,battery33.1C,severity0; CPU maximums2.1/2.7/2.9GHz,
GPU cap unavailable, Game Mode`standard`. Existing application ADPF hints and
automatic vendor VSPA thread-speed messages are observed; no manual mode or
system setting changes occur and cap causes are not investigated. Initial
1280x720 and1920x1080 targets are logged, but no sampled main-pass DRS or
BC-shadow series is available. Display-timing refresh16.591ms is logged;
actual presentation feedback and pair-warm counters remain unmeasured
(only four enabled workers announced).

The exempt notice names pool`0x79e300a000`, size`0x123000` =1,191,936B /
1.13671875MiB. First24 allocation messages reach cumulative copied162.6MiB,
zero-copy0.0MiB; these are neither live residency nor bytes copied each
submission. All14 startup slow-replay profiler samples print pool-sync0.0ms
rounded to0.1ms. No600-submission volume row is reached, so MiB/submission and
steady copy cost remain unknown; incremental dirty64KiB copies and aggregate
tracking/lock costs prevent estimating them from the notice pool's size.

Required safe end state **passes**: release3 e85e77ce installed; complete data,
bank a98538dc and manifest50534b64 retained and rechecked;17 owned-seed file
hashes match before an atomic rename into the ordinary save while stopped.
Debug/validation builds and private test files/roots are removed, app stopped,
screen OFF, timeout30000ms unchanged. Keeper/installer helper ended; no input
loop started. Tablet, PC production selectors and exported production APK stay
untouched. The new fix is committed as96f8a505; source hashes still match the
APK3 build and the182-test result.

`BENCH/astc-phone-validation.json` appends this third continuation and retains
all earlier attempts, APKs and blockers. It includes complete current rows,
criteria, source/build/census hashes, screenshot index (setup transition only),
audio/driver/exit logs, copy-cost limits and final state. **Publication remains
blocked by the unapproved audio RED and incomplete device acceptance**.
Next is supervisor review of that audio failure and a new supervised continuation,
without assuming any change to the approved acceptance. No merge, push, tag or
publication occurs.

## Step3 continuation: clarified V gate and FP32 blend failure (2026-11-03)

The supervisor clarifies that V is judged on zero Vulkan validation errors and
no crash. Warnings and recovered audio-watchdog REDs are recorded; non-validation
runs retain the strict RED0 gate except one zero-copy capability notice. This
scope supersedes the previous audio-only stop without changing APK3 or data.
The private harness records each audio/restart event with adjacent-read host
monotonic bounds and UTC observation times, retains epoch-timestamped logcat,
and polls keyguard/focus every5s. Launch requires unlocked/awake; a foreground
lock marks the run disturbed and permits one restart. Gate fixture checks V
audio/warnings are allowed while non-V audio and V validation errors still fail.

### New correctness blocker

Fresh validation3 starts with the layer enabled. Before title detection it
reports **2 errors**:

```text
VUID-VkGraphicsPipelineCreateInfo-renderPass-06041
blendEnable = VK_TRUE with VK_FORMAT_R32G32B32A32_SFLOAT,
whose format features lack VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT.
```

The layer's full reported format-feature masks are retained. Three identical
warnings concern a fragment output at location0 with no corresponding color
attachment; they are listed without failing V. Raw RED7 comprises baseline1,
recovered audio1, warnings3 and real validation errors2. No NonWritable06341
recurrence is observed. Required errors0, observed2: **V fails by2**.
The controller stops the process; no crash is observed, no new format fallback
or shader/runtime fix is attempted, and no further experiment follows.

One201ms audio watchdog is recorded at host observation16:22:18.188+08,
bounded by an adjacent-read interval0.688s wide. `AudioUnit restarted` is
observed16:22:19.477, interval0.719s wide. Logcat records dead IAudioTrack at
epoch1793694137.708 and AAudio disconnection at1793694137.736; the new stream
s#2 opens successfully (`AAUDIO_OK`) at1793694138.969. This is restart/reopen
evidence, not an exact native timestamp for the file's watchdog line or proof
of audible output. No later2000-callback diagnostic is reached before the
unrelated Vulkan error. Audio is **not the blocker in this V run**.

| Approved item | Current result |
|---|---|
| V, zero errors/no crash | FAIL:2 errors,3 warnings; controller stop, no crash observed |
| Four-view comparison and BC5 normals | Not reached |
| Sidecar hits/misses/exceptions, fetch distribution/frame correlation | Not run; no600-present counters, native phone probe never pushed |
| Isolated save/reload +30s smoke | Not run |
| 120s moving route/60s warmup and600s continuous | Not run; fps,>17.5%,p99,>50/>100,max and memory/LMK acceptance unknown |
| Same-debug OFF/ON | Neither run; all no-regression limits unevaluated |

### Data audit, disturbance and safe end

The controller mistakenly overlaps the initial hash audit and V startup.
Only `env.txt` and `settings.txt` differ after V writes the intended handheld
test configuration; all immutable data files match. This invalidates that
configuration audit and adds startup load, so no timing/performance inference
is drawn. The app is stopped, original preferences restored, and the **full
48,182-file audit reruns serially:0 mismatches,111.718s**, followed by **32/32
installed sidecar chunk hashes**. No bulk data is resent. The launch controller
now requires completed full-file/chunk proof before preparing any app run.
Both the overlapping audit and corrected proof are retained. The explicit
format-feature validation error remains the correctness blocker.

All active keyguard/focus samples show unlocked GameNativeActivity foreground;
Settings was focused before launch. No lock disturbance, restart or transport
disconnect is observed. Startup PID23057 has RSS1,442,480KiB (1408.672MiB),
PSS1,269,629KiB (1239.872MiB); HAL CPU/GPU73.628C,skin36.786C,battery33.3C,
severity0; CPU caps2.1/2.6/2.6GHz, Game Mode`standard`. No mode/performance
setting is changed by the agent. Main-pass DRS, BC-shadow residency over time,
GOOGLE presentation feedback and pair-warm counters remain unmeasured; four
workers and refresh timing are merely announced.

The once-exempt pool is1,191,936B/1.13671875MiB; the first24 allocation logs
reach cumulative copied162.6MiB,zero-copy0.0. All22 startup slow-replay sync
samples print0.0ms rounded to0.1ms. No600-submission volume row exists, so live
copy residency, MiB/submission and steady copying cost remain unestablished.

Safe end is verified again: exact release3 e85e77ce, full data, bank a98538dc
and manifest50534b64;17 staged seed-file hashes match before atomic rename
into the ordinary save. Debug/validation/test roots are removed, app stopped,
keeper ended, screen OFF (sleep reissued after an awake readback). The current
screen timeout reads**600000ms**, differing from the prior attempt's30000ms.
The agent issued no settings writes; this observed external change is retained
without attributing its source or changing it. Performance mode stays standard.

`BENCH/astc-phone-validation.json` appends the fourth continuation and preserves
all previous attempts. Evidence under
`astc-phone-validation/session-20261103-step3-vgate/` includes audio recovery
timestamps, raw warnings/errors, keyguard/focus transitions, both audits,
32-chunk proof and final state. APK/source hashes still match the existing
build and182-test result; no rebuild or production export change occurs.
Publication is blocked by VUID06041 and the unexecuted acceptance. Next is
supervisor review/correctness work on the unsupported blend format, followed by
a supervised continuation. No merge, push, tag or publication occurs.

## FP32 blend investigation: partial evidence, no selected fix (2026-11-03)

The user chooses investigation before a fix and permits debug-only env-gated
prototypes. **No runtime default or production artifact is changed.** The user
also confirms personally choosing screen timeout600000ms; retain it. This
confirmation supersedes the earlier unknown attribution without rewriting
that historical observation. Related upstream searches after fetching
nx2android34333894 and nxnative fa681f94 find no existing format-blend fix.

### Pipeline identification

The debug probe logs full pipeline-key hashes, VS/FS keys, every MRT format,
blend factors/equations/masks and, on an actual matching draw, present/pass,
dimensions, image usage, NVN format/flags and storage identity. It is compiled
out of release and inactive without `DM_VK_X_FP32_BLEND=log` or `=C`.

The original1209-key PPE2 cache (1000B keys, SHAd6b47da4) contains **3 affected
pipelines /4 attachment states**. Exact production-hash matches prove all
three observed invalid creations are cached prewarm keys. This is more
specific than assuming an observed creation names a current render target.

| Pipeline hash | VS | FS | MRT / blend state |
|---|---|---|---|
| `4d97fe7b0d8ee31e` | `b2c0cfbcd9722ace` | `58606cb67cb94e6d` | one RGBA32F: RGB SrcAlpha/One/Add; alpha Zero/OneMinusSrcAlpha/Add |
| `8c998753f3c99d14` | `b2c0cfbcd9722ace` | `f84705cb4c5a46b8` | same one-target state |
| `24621523461e6567` | `590001dbe0f4877a` | `2a6b54a6274c39c5` | two RGBA32F: RGB and alpha One/One/Add on both |

All have samples1, depth format undefined and RGBA write masks. The first two
compute `RGB=S.rgb*S.a+D.rgb`, `alpha=D.a*(1-S.a)`; the third adds source and
destination. These are pipeline-state equations, not an identified game effect.
**Zero corresponding actual draw attempts are observed before the loading
stop.** Physical target size, usage, pass ordinal/name, scene effect and draws
per gameplay frame remain unknown. The cache lacks image dimensions/usage;
no target is invented from its format. No R32F/RG32F blended pipeline occurs in
these1209 inputs or the observed startup; this is not a whole-game census.

### Phone format and extension properties

Optimal-tiling features from the saved phone report:

| Format | Color attachment | Blend | Linear sample filter |
|---|---|---|---|
| R16F / RG16F / RGBA16F | yes | yes | yes |
| R32F / RG32F / RGBA32F | yes | no | yes |
| RGB32F | no | no | yes |
| RGBA16_UNORM | yes | yes | **no** |

Other supported color attachments lacking blend are R8/RG8/RGBA8 UINT/SINT,
A8B8G8R8 UINT/SINT, A2B10G10R10 UINT, R16/RG16/RGBA16 UINT/SINT, RGB16 SINT,
R32/RG32/RGBA32 UINT/SINT; R8/RG8/RGB8/BGR8 SRGB and A8_UNORM. Together with
the three float32 color formats, these are30 entries. The exact enum names,
numeric values and feature masks are in `format-support.json`. The runtime
already excludes integer formats when constructing blend state.

`VK_EXT_shader_tile_image` is advertised with color/depth/stencil read1 and
coherent-read-accelerated1. `VK_EXT_rasterization_order_attachment_access`
reports color/depth/stencil access1; the advertised older ARM alias's saved
feature bits read0. `VK_KHR_dynamic_rendering_local_read` reports1.
`VK_EXT_fragment_shader_interlock` is absent. These are saved supported
features, not features enabled by the existing runtime for software blending.

### A/B/C comparison and limits

| Option | Evidence / precision | Work and risk |
|---|---|---|
| A: RGBA16F substitution | Not implemented or run after the new correctness blocker. Range/precision consequences known, actual target values unknown. | Estimate2-4 engineer-days for a coordinated scoped prototype after identifying images; moderate/high risk until aliases and CPU traffic are known. |
| B: FP32 shader blending | Feasible hardware paths reported; not a small local prototype, not implemented or run. Keeps FP32 storage/arithmetic in principle. | Estimate3-5 engineer-days for constrained implementation/validation; high synchronization, shader-variant and MRT risk. |
| C: blend disabled | Implemented only in explicit debug mode;06041 disappears. It changes no image/view/copy/sampling formats. No successful scene comparison or performance result. | Reference only, never a fix; can lose destination accumulation/compositing. |

A would store8 rather than16 raw bytes/texel, with11 rather than24 significant
bits, finite max65504 and smallest subnormal2^-24. Near1, its ULP is2^-10,
versus2^-23 for FP32. Normal round-to-nearest relative error per store can be
about2^-11; overflow, underflow and loss of small terms in repeated additive
blends matter beyond that bound. FP32 shader calculations do not prevent
FP16 attachment rounding. Actual target range, feedback behavior and CPU use
were not captured, so no quality or memory/FPS acceptance is claimed.

A correct image prototype must coordinate physical image format and all
attachment/sampled views, compatible framebuffer lists, aliases, deferred
replacement and copies. Guest FP32 byte layout stays16B/texel; CPU uploads,
buffer transfers and readbacks require explicit conversion/row handling.
Changing only pipeline attachment formats or one view is insufficient. No
unsafe global format remap or unimplemented A selector is retained.

For B, [tile-image reads](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_shader_tile_image.html)
can read previous color values in rasterization order but require dynamic
rendering; this device currently uses classic imageless render passes.
A prototype needs feature negotiation, a supported dynamic path, and
[SPIR-V tile-image declarations/read instructions](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_shader_tile_image.html)
with blend arithmetic before fragment outputs. Variant/cache keys must account
for blend state, masks and MRT; depth variants, partial outputs, sample/helper
behavior and ordering must be checked. An alternative uses
[raster-order input attachments](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_rasterization_order_attachment_access.html),
requiring render-pass/input-attachment usage and descriptor changes. Simple
ordinary sampled reads of the same writable image are not an equivalent
ordered-blending implementation. The engineering estimates are judgments for
a constrained prototype, not measured completion times or shipping commitments.

### C run: new blocker and visual evidence

C validation gets past the prior06041 stop and the draw-count title detector.
The first attempt stops on a private harness PID-path bug; the corrected
attempt restores a fresh isolated seed. During loading it encounters **3
`VUID-vkCmdDrawIndexed-magFilter-04553` errors**: bindings1/2
`fp_t_tcb_8`/`fp_t_tcb_A` use linear sampling of RGBA16_UNORM, which lacks the
required filter feature. **Four warnings** are retained: three unused fragment
outputs and one UINT-output/FLOAT-attachment mismatch. Recovered audio events
and keyguard/focus logs are preserved under the clarified V gate. No further
device experiment follows this real correctness failure.

All phone `title.png` and `load-0/1/2.png` files are pixel-identical: black
game area with the controller overlay visible. Manual review therefore rejects
the detector labels as proof of rendered title/load. The tablet reference
shows the normal title. A central phone crop, pixels1120,252..1680,819, is
exact RGB0 throughout. This is evidence of the black capture, **not an FP32
target readback or proof that disabling blend caused it**.

Private `C-versus-tablet-title.png` places the screenshots side by side without
enhancement; `C-game-area-crop.png` preserves exact crop pixels. Original sizes
are2800x1260 phone and3040x1904 tablet. The phone uses handheld/stock DRS
(main pass not sampled); tablet references are docked/stock DRS, whose earlier
route sampled1600x900. There are no matched A/C Plateau/end images, measured
target precision ranges, normal-map acceptance or meaningful performance
comparison. C does not satisfy the requested zero-new-VUID condition.

### Checks, end state and recommendation

Full suite **183 PASS,0 skipped,143.360s**; debug and release each compile157
sources with0 failures. Native tests verify default/log state preservation,
C's unsupported attachment only, unchanged keys and release gate0. An existing
descriptor-hash fixture is corrected to extract the function definition rather
than the newly added forward declaration. Only debug diagnostics/C reference
are committed; the release compile check is not installed or packaged.

Exact release3 e85e77ce is restored with complete data, bank a98538dc,
manifest50534b64 and17 verified seed files renamed into the ordinary save.
Prototype builds/files/roots are removed from the phone, app stopped,
screen OFF, keeper ended. The user's600000ms timeout is retained. The original
APKs, pack, production selectors and export are unchanged. The measured C
build precedes removal of an unused/unimplemented A selector; its earlier
header is reconstructed from the logged edit and archived for provenance.
C/log/default/release behavior is unchanged by that cleanup.

`BENCH/astc-phone-validation.json` appends this partial investigation, retaining
all earlier attempts and original release3 acceptance rows. Exact cache rows,
format/extension masks, shader summary, raw errors, audio/focus records,
screenshots/crop/comparison and end proof are under
`astc-phone-validation/session-20261103-fp32-investigation/`.

**Recommendation:** do not select a lossy default format change from prewarm
records alone. First investigate the separate04553/filterability and black
game capture, then identify physical FP32 demand targets and resume controlled
A/C (and B if warranted). The user must choose whether to authorize that
additional diagnostic scope before choosing FP16 versus FP32 engineering.
The requested comparison remains incomplete; no fix is selected or shipped,
and no merge, push, tag or publication occurs.

## Mali compatibility findings

### 2026-11-03: black-output matrix and first failing pass (current)

**Confirmed first blocker: missing GL depth-convention conversion on devices
without `VK_EXT_depth_clip_control`. This is not a complete-game fix.** A private,
hash-verified replacement for the final vertex shader restores the title, load
menus and gameplay HUD. The Great Plateau world remains almost entirely black,
with persistent Mali GPU task errors. No default, generated pack or release APK
changes are made; A/B remain undecided and phone acceptance remains blocked.

The user explicitly directs this investigation and permits recovered audio
watchdogs plus the approved once/process zero-copy notice. This supersedes the
previous continuation's strict diagnostic stop only; non-validation acceptance
still requires RED0 apart from the approved zero-copy notice. Scope remains owned
BotW1.9.0/v1114112+DLC, vivo V2405A/MT6991/Immortalis-G925, default balanced,
handheld, stock internal DRS, target60, HOLD off, audio on and a fresh isolated
copy of the17-file owned Plateau seed for every launch. The observed title main
pass is1280x720 and guest window1920x1080. No vendor settings are changed. The
600000ms timeout is the user's earlier change and is retained.

Private evidence is under `BENCH/astc-phone-validation/session-20261103-black-matrix`.
`parse-summary.json` indexes18 attempts,17 launches,110 internal PPMs, all PNGs,
per-run counters/REDs/audio/focus/memory/thermal and exact hashes. The appended
`BENCH/astc-phone-validation.json` preserves both previous investigations and
all prior acceptance rows. These are correctness diagnostics, not FPS experiments.

#### Format diagnostic matrix

All four completed conditions use the same private debug APK, SHA-256
`dc243617e7b6c62349f7c0ae4604b468e763e07ff649fa37874c5f249a0d721f`.
Each has9 PPMs including initial presents and corresponding title, three load
checkpoints, and seed-gameplay start/still workloads. The scene detector uses
draw counts; a black image is not positive visual proof of the Plateau view.

| Condition | Private run | Internal result | Last600-frame workload |
|---|---|---|---|
| Baseline | BLACK-BASE-R1 | 9/9 RGB exactly0 | 3729 draws/frame |
| A: unsupported blended RGBA32F through FP16 | BLACK-A-R1 | 9/9 RGB exactly0 | 3738 draws/frame |
| U: unfilterable RGBA16_UNORM through nearest | BLACK-U | 9/9 RGB exactly0 | 3738 draws/frame |
| A+U | BLACK-AU | 9/9 RGB exactly0 | 3699 draws/frame,166.20 passes/present |

A is a debug-only, capability-gated FP16 attachment proxy with typed GPU blits
before/after the pass. Ordinary views, copies and CPU byte layout keep the
canonical FP32 image. Only the observed single-sample2D/single-layer shape is
covered. In AU,270 sampled attachment logs identify two8x8 demand targets with
VS `b2c0cfbcd9722ace` and FS `58606cb67cb94e6d` / `f84705cb4c5a46b8`.
The third cached/prewarmed pair, VS `590001dbe0f4877a` / FS `2a6b54a6274c39c5`,
is not observed as a demand draw in these captures. AU logs no remaining demand
FP32-blend attachments. This does not prove all scenes are covered.

U checks the actual view format on both fresh and cached descriptors. The
original04553 path is guest format/view `0x2a` -> VkFormat91 RGBA16_UNORM,
256x256, usage `0x17`, FS `13540660ad78e484`, bindings1/2. Additional vertex-stage
keys and sizes are in the JSON. AU snapshots resolve37/40 descriptors on the
unfilterable format and replace37/40 samplers with nearest min/mag/mipmap and
anisotropy1. Those are descriptor-resolution counts, not GPU texture-fetch counts.
A loses FP32 range/precision (finite max65504,11 versus24 significant bits,
rounding on both proxy conversions); U removes interpolation/anisotropy. Neither
quality tradeoff is approved for production. No new validation-layer run of
these prototypes is claimed.

#### The first black pass and a causal depth diagnostic

`present_window()` in `gfx_vk_present.c` blits the game image into `g_simg[idx]`
then calls `ov_render()` into that same swapchain image, using LOAD. Own PPMs
read the game image before the overlay. The old overlay-visible black captures
therefore cannot be explained by hiding a separate secure game layer.

| Stage | Image / state | Evidence |
|---|---|---|
| Title pass63 | 1280x720 A2B10G10R10_UNORM_PACK32 (64), recognizable complete title/menu | `BLACK-STAGE/passes-0/pass_63.ppm`, lossless `pass_63.png` |
| Final pass64 | 1920x1080 RGBA8_SRGB view43 of RGBA8_UNORM image37;1280x720 viewport; RGB0 | `BLACK-STAGE/passes-0/pass_64.ppm` |
| Draw | VS `e4b5b0967a3cd0b3`, FS `ecb4242efaf4042b`; two indexed triangles; blend/depth disabled; source is pass63 texture ID772 | `BLACK-STAGE/final.log` and targeted UBO/bind traces |
| Clip input | `clip_neg1=1`; source vertices z0, CBUF80B maps all vertices to z=-1,w=1; device lacks depth_clip_control | `clip-cache.txt`, owned shader disassembly and trace |
| Clean replacement control | Original computation, same capability/output cleanup; final PPM remains RGB0 | `BLACK-CLIP-CONTROL-R2` |
| Depth replacement | Only final VS changes z to0.5*(z+w), FP32 throughout; pass64 and final title become visible | `BLACK-CLIP-R2/frame_001342.ppm` and `passes-0/pass_64.ppm` |

Vulkan's default clip volume rejects negative z; the optional negativeOneToOne
state extends it to -w. See the [Khronos clipping specification](https://docs.vulkan.org/spec/latest/chapters/vertexpostproc.html#vertexpostproc-clipping)
and [depth-clip-control state](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineViewportDepthClipControlCreateInfoEXT.html).
The backend currently leaves GL depth unchanged when the extension is absent
(`gfx_vk_exec.c`, `set_dynamic()` TODO). Thus the final quad is entirely clipped.
The same missing fallback exists upstream nx2android
`34333894c5192606d1ac88b10e8e040667d556c8`, file
`nxnative/dm/android/gfx_vk_exec.c:1265`. The freshly fetched nxnative
`b5910377e5bd052a080affaa947ca83c80cf4a8c` already expresses the required
coordinate conversion in its Metal `vs_depth` design,
`dm/gfx/tools/dmsh_build.py:1011-1021`. This is the relevant upstream semantic
reference; no Unreal implementation is used or copied.

Both private SPIR-V modules pass `spirv-val --target-env vulkan1.1`. Their
internal phone files are read back and hashed, and the runtime explicitly logs
loading the alternative module. Clean control SHA:
`7a7693878195cd902b608f8b5924072ae522af6593bcd2562006ad5133404e2e`;
remap SHA:
`7f7c2e4846d7cee210be582a85288c035bbd391e69405c1752ab93a81f131701`.
The initial BLACK-CLIP/CONTROL external binary writes were incomplete and did
not load; those attempts are inconclusive and preserved. CONTROL-R1 aborted
before launch on a read/hash assertion (run-as cannot read that external FUSE
path). Corrected R2 uses binary-safe `exec-in run-as dd` and internal read/hash.
No runtime or data-pack change was needed for this shader diagnostic.

A fresh-seed AU+depth run, `BLACK-CLIP-GAME`, restores the load menu and three
red HUD hearts. Its final frame5011 has only0.05917% nonzero RGB across the
1920x1080 canvas: the world still fails. It records2655 GPU `GROUP_ERROR_FATAL`
messages, final600-frame GPU time0.00ms,3706 draws/frame and166.14 passes/present,
with no program/target/pipeline skips. PID remains stable until controller stop.
These GPU task errors are a rendering blocker, not evidence of an application
crash or LMK. The one-VS proof does not establish why the remaining world is
black: other GL-depth stages and the GPU task failure require further isolation.
No further phone experiment follows that evidence.

#### Other controls, audio, limits and end state

Startup no-cull, encode-thread off, and selected descriptor-cache/dynamic-UBO
fallback controls all retain the black title. The live no-cull toggle in the
first pass-dump run was ineffective (startup-cached); the separate startup run
proves cull0. Disabling AHB import intentionally takes the copied-pool path
(-4 diagnostic notice) and remains black. A deliberately missing private bank
selects the existing reject/BC-decode fallback and remains black. Ordinary-bank
rows show0 lookups/hits/misses/exceptions at title, so the title failure precedes
any sidecar fetch. This does not exonerate every descriptor or full-game ASTC
path and does not establish sidecar coverage/fetch timing.

Across17 launches,52 audio watchdogs (200-203ms) have52 successful AudioUnit
restarts;51 also have subsequent existing2000-callback evidence. All timestamps
and adjacent host-read bounds are recorded.14 are within5s of first observing
game focus; the other38 occur without an active focus change. No sampled active
keyguard/foreground loss occurs. This does not establish audio causation. Recovered audio still fails strict non-validation acceptance; the zero-copy
notice remains permitted only once per process under its existing exception.
The diagnostic allowance is not expanded.

The approved pool is `0x79e300a000 +0x123000` (1,191,936B/1.13671875MiB).
AU existing counters show2301.3-2310.9 printed MB over600 submissions,
3.84-3.85 per submission (the code divides by1048576: MiB despite its MB label).
There is no isolated pool-copy timer in these measured APKs; no0ms cost claim.
Memory/thermal/cap series are preserved in each run's JSON/raw files; no FPS,
thermal comparison, no-regression result or retained-memory acceptance follows
from faulty, heavily instrumented rendering. BC5 normals and a matching
Plateau/route-end image cannot be judged. Tablet title labels/layout are
recognizable after the remap; slideshow/fade phases differ, with tablet3040x1904
captures/docked DRS versus phone2800x1260/handheld and1280x720 sampled main pass.

Full host suite: **184 PASS,0 skipped**,170.262s (172.578s wall). Debug and
release compile157 sources each,0 failures; release library excludes both new
prototype selectors. Release compile is a host gate check only. The new
validation companion SHA is
`e7ac632477b5ee60c0eac039b9c56d4ca8a13149affb7e2c549ec707d886a31b`
and is not run. No APK3 release or generated data is rebuilt.

End proof: original release3
`e85e77ce04aedb012b5e536fec30ed50542d0694e6bea56e66213aebb41234c1`
installed,48,182 installed-file hashes/32 sidecar chunks verified,0 mismatches,
0 bulk bytes retransferred. The full audit temporarily restores pack env/settings
while the app is stopped; final requested handheld preferences are then restored.
17 seed files are hash-verified then restored into
ordinary save. Internal/external test roots, probes, captures, controls and
validation/debug build content are removed. Both local helpers stop; a cleanup
race causes six read retries for an already removed installer XML (not USB
loss), explicitly recorded with19,232,312,322 retained bytes. A subsequent
read-only check finds the screen awake, origin unestablished; sleep is repeated
after both helpers stop. The18:32:46 settled check confirms exact release3,
app stopped, screen OFF and user's600000ms timeout. No measured disconnect,
account/system-setting change, merge, push, tag or publication occurs.

After the completed safe end and commit, an optional read-only recheck at
18:39:09 confirms the app stopped, then loses the same serial during the power
query. The device does not reappear within the approved600s wait. This is a
post-cleanup transport loss: no measurement or data transfer is active and no
run is invalidated. The last complete observed end remains18:32:46 release3,
seed/data retained, app stopped/OFF/600000ms; the state after disconnect cannot be
re-observed. All retry/absence/failure timestamps and retained bytes are appended
to the same BENCH JSON and transport log. No further device action follows.

**Next:** implement and test a device-gated, semantics-preserving FP32 GL depth
fallback for every relevant final pre-rasterization stage/variant, including
geometry/tessellation emission and transform-feedback implications, then
isolate the remaining world/GPU failure. Correct06041 and04553 independently
and rerun validation before visual/performance acceptance. The confirmed depth
bug requires no FP16/nearest quality concession; A/B decisions need working
scene evidence and explicit approval before any production precision change.

### 2026-11-03: UE reference, 04553 and capture attempt

**Status: partial investigation, stopped by the existing non-validation audio
gate.** No runtime source, default, game profile, generated data or APK changes
in this continuation. One bounded non-validation attempt reused the existing
debug C reference. It stopped on an audio watchdog RED before the title-ready
checkpoint and before any internal PPM. Black rendering versus excluded screen
capture remains **unresolved**; the earlier black screencaps do not prove that
BotW's render target is black. This continuation does not select A or B.

The measurable scope was owned BotW1.9.0/v1114112+DLC, V2405A/Dimensity9400,
balanced, handheld, stock internal DRS, target60, HOLD off, audio on, isolated
Great Plateau seed. The diagnostic capture goal was corresponding application
PPM/system PNG at title, load and Plateau, with strict unapproved RED0, stable
PID and unlocked foreground. No main-pass size or performance threshold was
evaluated before the stop. The user's600000ms timeout is retained as an
explicit **user change**, not attributed to this agent.

### Read-only Unreal Engine design reference

Existing authenticated `gh api` access pins EpicGames/UnrealEngine
**5.8.4-release**, commit `f45729c0b6f097038e72b52d366a62baeca3275b`
(2026-10-20). Only relevant VulkanRHI and Android platform files were read.
The following is an independent description, not copied implementation;
no Unreal source is added to the repository or private artifacts. Links
require the reader's Epic/GitHub entitlement. References below share prefix
`Engine/Source/` at that commit.

| Topic | Observed UE mechanism | File / lines | Applicability and limits |
|---|---|---|---|
| RGBA32F without blending | Maps the abstract RGBA32F format directly to RGBA32F. Records renderability and blendability separately. The reviewed pipeline serializer writes the requested blend state. | [Runtime/VulkanRHI/Private/VulkanDevice.cpp:763-772,1152-1168](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp#L763); [VulkanPipeline.cpp:807-828](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp#L807) | No automatic FP32-to-FP16 blend substitution, blend disabling or shader emulation found in these paths. A capability flag is useful; it does not implement the guest's unsupported blend operation. Higher-level renderer policy was outside this read scope. |
| RGBA16_UNORM without linear filtering | Has ordered UNORM16 then FP16 candidates; chooses the first satisfying required capabilities. The default requirement is sampling, **not filtering**. Filterability is recorded separately. | [VulkanDevice.cpp:799-800,1123-1218,1221-1264](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp#L799) | Missing LINEAR alone does not force substitution in this mapping. Do not cite UE as proof that it always converts Mali UNORM16 to FP16. |
| Samplers / Android R16 | Sampler construction translates the requested filter without an image-format argument. Android explicitly prefers R16F for the single-channel G16 format for cross-GLES compatibility. | [VulkanState.cpp:197-222](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/VulkanState.cpp#L197); [VulkanDevice.cpp:874-881](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp#L874); [Android/VulkanAndroidPlatform.h:137-141](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/Android/VulkanAndroidPlatform.h#L137) | No general image-dependent nearest-filter downgrade in this sampler path. The single-channel Android policy is not a four-channel precision guarantee. |
| Android compression selection | Separate ASTC/ETC2 cook variants, configurable priority, extension/GLES checks when selecting a device target, format remaps and optional multiple cooked representations. Vulkan maps each compressed format through format support; HDR is extension-gated separately. | [Developer/Android/AndroidTargetPlatformControls/Private/AndroidTargetPlatformControls.h:191-200,226-235,251-286](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Developer/Android/AndroidTargetPlatformControls/Private/AndroidTargetPlatformControls.h#L191); [AndroidTargetPlatformControls.cpp:542-601,628-668](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Developer/Android/AndroidTargetPlatformControls/Private/AndroidTargetPlatformControls.cpp#L542); [VulkanDevice.cpp:847-889](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp#L847) | Our per-device pack is an offline BC conversion with hashed sidecar lookup and retained BC shadow, not UE cooking from original assets. UE's GL target-selection check is distinct from Vulkan's actual format-property check. |
| Android/Mali memory workarounds | Probes image allocation sizes with alternate creation flags; selects an AFBC memory workaround when measured allocation savings warrant it. A separate ASTC allocation probe can select linear tiling for small mipmapped images. | [Android/VulkanAndroidPlatform.cpp:2170-2310](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/Android/VulkanAndroidPlatform.cpp#L2170) | Relevant design for measuring retained/image memory later; neither a blend nor filtering fix. No new workaround is enabled here. |
| AHardwareBuffer | Queries an external hardware-buffer image's properties, imports it with dedicated image allocation and the returned allocation size/type bits, then uses image/Y'CbCr resources. | [VulkanTexture.cpp:2065-2161](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp#L2065) | Different from importing our guest memory pools as buffers. No applicable Mali guest-pool zero-copy fallback repair found. |

Bounded searches of the reviewed VulkanDevice/Extensions/Shaders and Android
files find no Mali-specific NonWritable decoration repair, FP32 blend emulation
or UNORM16 filtering repair. This is not an engine-wide absence claim. Existing
local NonWritable fix96f8a505 remains separate. Targeted upstream searches after
fetching nx2android34333894c5192606d1ac88b10e8e040667d556c8 and
nxnative7ef2aaf54ad63747623ae044f2963e310a031f11 find no directly reusable
related correction. nx2android already forces nearest for integer views; that
does not cover this UNORM view.

Compared with UE, useful gaps to evaluate in our sidecar path are a general
per-format/per-usage capability contract (blend, filter and image shape, not
just BC/ASTC family), an ETC2/EAC deployment fallback for devices lacking the
required ASTC decode mode, per-shape compressed-image checks (UE specifically
falls back from ASTC for volume textures), and source-aware normal/HDR encoding
policy. UE's cook path can use original normal-map intent; a BC transcode must
preserve already encoded channels and signedness. Our existing manifest gates,
mixed exceptions and per-texture hashes remain valuable and unchanged. These
are design gaps or follow-up checks, not assertions of new phone failures or
permission to expand this investigation.

### 04553 identification and minimal handling proposal

The preserved C validation log has **3 error reports**, all
`VUID-vkCmdDrawIndexed-magFilter-04553`, while loading:

| Object | Observed evidence |
|---|---|
| Descriptor set | `0x2cb20000002cb2`, Set1, Index0 |
| Sampled view | Both bindings use `0x2cb10000002cb1`, `VK_FORMAT_R16G16B16A16_UNORM` (91) |
| FS binding1 | `fp_t_tcb_8`, sampler `0x2c6a0000002c6a` |
| FS binding2 | `fp_t_tcb_A`, sampler `0x2b710000002b71` |
| Sampling | LINEAR min or mag, comparison disabled; physical format has no SAMPLED_IMAGE_FILTER_LINEAR |
| Still unknown | Image handle, dimensions, full usage, guest texture/view IDs, full sampler state, exact VS/FS keys, pass and draws/frame. Three validation reports are **not** a draw-frequency measurement. |

Source provenance narrows the cause: `gfx_vk_tex.c:44,60` maps guest formats
0x2a/0x72 to this UNORM16 format; `color_image_require` changes usage/creation
support without reducing the format, comparison clones use D16/D32, and BC6H
CPU fallback uses RGBA16F. Thus this sampled **view format follows a guest
format or guest reinterpretation view**, rather than an ASTC decode, D16 clone
or color-promotion-to-UNORM16 policy. The particular image's creator/alias is
not established by the old log. Do not infer its usage flags or shader keys
from nearby asynchronous shader-compilation lines.

`gfx_vk_exec.c:1340-1368` selects nearest only when `view_num` classifies an
integer view. UNORM16 is float-class, so it retains the requested linear
sampler. The correct compatibility contract must check the **actual view
format's optimal-tiling feature bits**, including cached descriptors, before
using that sampler. Merely checking the image parent's format misses aliases.

The smallest diagnostic prototype would explicitly gate nearest min/mag,
nearest mip selection and anisotropy1 for unsupported view formats while
preserving wrap/LOD/compare semantics and sampler snapshot versions. Existing
`vk_samp_at_mode` supplies much of that mechanism. It would remove this illegal
linear request, preserve the UNORM16 stored values, and permit a controlled
comparison, but **changes filtering quality**; it is not a demonstrated
semantics-preserving production fix. It was neither added nor run after the
strict non-V stop.

To preserve the game's requested filtering, prefer a narrowly scoped
shader-side filter path over a supported nearest sampler (four taps per
bilinear sample, with correct address/LOD/mip handling); shader variants,
implicit derivatives and performance still need evaluation. An FP16 physical
substitute can use hardware filtering and keeps8B/texel, but UNORM16 bits must
be converted on uploads/readbacks and all image/view/copy uses must agree.
UNORM16 steps are1/65535 throughout[0,1]; FP16 has11 significant bits and a
step of1/2048 just below1, about32 times coarser. Reinterpreting the same bits
as FP16 is incorrect. UE's candidate fallback does not waive these guest-data
or precision obligations. No proposal has a visual or VUID-free phone result.

### Black-output attempt and strict stop

The existing debug C APK (`b2fb7bcd…`, no validation layer) was installed over
the retained complete data; no data retransmission or new APK build. Fresh
isolated seed and existing logical prewarm input were restored. Requested
PPMs were to use `DM_VK_SHOTS=1` at each scene checkpoint and a private writable
`DM_VK_SHOT_DIR`, alongside screencap. `gfx_vk_present.c:904-923` reads the
game's presented image before the screen overlay using an image-to-buffer
copy, so a successful nonblack PPM would distinguish rendering from an
OS-excluded layer. No window security flags are altered.

PID20138 reaches at least present158 but stops during title preparation:
audio#0 reports no callback200ms, observed **17:08:38.879663+08**, bracketed by
host monotonic544705843000000..544706562000000ns. Logcat records dead
IAudioTrack at epoch1793696918.216 and AAUDIO_ERROR_DISCONNECTED(-899) at
1793696918.238. No restart or subsequent callback evidence appears before the
controller stop. Raw RED2 = approved zero-copy notice1 + unapproved audio1.
The layer is off and Vulkan error count0 is **not** a validation pass. The
private preparation retained slow-replay diagnostics; this short diagnostic
failure is not a release/OFF/ON performance or audio-baseline measurement.

The title-ready detector had not returned, so no DM_CTL shot request was
sent: **0 PPM and0 new scene PNG files**. The user-reported FLAG_SECURE remains
a capture-exclusion hypothesis, not independently verified in a foreground
window dump in this run; keyguard `secure=true` alone would not establish a
window FLAG_SECURE. Earlier title/load PNGs remain capture evidence only.
There is no basis yet to identify a first failing rendering stage or blame
descriptor fallback, ASTC or pool copying. Sidecar-disable/descriptor-path
experiments were not started after this stop. No audio workaround, relaxed
gate or relaunch was introduced.

All active focus observations show the game foreground and unlocked; no
transport disconnect or disturbed-run restart. One startup RSS/PSS sample is
1,392,196/1,224,673KiB; automatic CPU caps sampled2.4/3.0/3.1GHz. These are not
growth or steady-state statistics; vendor/system modes were not changed.
The one fallback pool is0x79e300a000+0x123000 (1,191,936B/1.13671875MiB).
First24 allocation logs reach cumulative copied162.6MiB, zero-copy0.0MiB;
three slow-replay samples print pool-sync0.0ms rounded. Neither allocated
totals nor rounded timings determine actual MB copied/submission or cost.

### Recommendation, records and end state

UE does not provide evidence for an automatic FP16 solution to06041. Keep B
as the precision-preserving candidate if FP32 semantics are required; A
remains a simpler potential tradeoff only after actual target range,
aliases/readbacks and controlled visuals are known and the user chooses
reduced storage precision. Existing2-4-day A /3-5-day constrained B estimates
remain estimates, not fresh prototype results. Neither is selected or shipped.

Priority: (1) obtain a user decision on the strict non-V audio blocker;
(2) complete internal PPM versus screencap evidence; (3) actual-view04553
binding diagnostics and filtering proposal validation; (4) identify demand
FP32 targets and compare06041 A/C/B; (5) resume the original validation,
visual/BC5, sidecar/fetch, save/reload,120s/600s and OFF/ON acceptance. Pool-copy
cost remains a recorded baseline/performance question, not a proven black
rendering cause. No additional phone experiment is authorized by this list.

`BENCH/astc-phone-validation.json` appends this investigation and preserves
all old rows, attempts, images and blockers. New own-word UE references,
failed-run logs/events/focus, zero-frame index, host checks and end proof are
under `astc-phone-validation/session-20261103-mali-compat/`.
Exact original release3 `e85e77ce…` is reinstalled/hash-verified, bank a98538dc
and manifest50534b64 rechecked,17 seed-file hashes verified before atomic
replacement of the ordinary save. Probe build/test roots and files are
removed, keeper ended, app stopped, screen OFF; timeout600000ms retained.
This documentation continuation makes no runtime/porter change and does not
merge, push, tag or publish anything.

Host verification before this documentation commit: full
`python -m unittest discover tests` **183 PASS,0 skipped,151.060s**
(152.500s wall), with native compiler/NDK dependencies available. No new
runtime prototype or APK was built; prior debug/release compile evidence is
retained, not relabelled as a new build.

Post-restore transport note: an optional read-only recheck lost adb at
17:16:57+08, after the17:09:57 release/data/seed/stopped/OFF proof. Retried
with19232312322 retained bytes recorded; same serial returned as transport14
after565.589s, within the600s tolerance. A first immediate terminal assertion
failed without saved field values; the settled repeat at17:26:45.957+08
verifies exact release3, stopped, screen OFF and timeout600000ms. No new app
launch or measurement; this is separate from the run's zero disconnects.
