# Zstandard decompressor

Unmodified common/decompress sources from facebook/zstd v1.5.7, pinned by
`sources.json` (Git blob and SHA-256 for every file). Only the decoder is linked
into the Android host. This port uses the BSD-3-Clause option in `LICENSE`.
No compressor, program, toolchain or game data is included here.

The independent image frames contain all ASTC bytes precomputed on the desktop;
the phone only decompresses them and verifies their SHA-256 before upload.
