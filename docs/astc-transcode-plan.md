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
