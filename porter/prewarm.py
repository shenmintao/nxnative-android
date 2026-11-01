"""Find a game's optional recorded pipeline keys by the packed main NSO build ID."""
from pathlib import Path


def find_keys(android_work, main):
    with Path(main).open('rb') as stream:
        header = stream.read(0x60)
    if len(header) != 0x60 or header[:4] != b'NSO0':
        raise ValueError('pipeline prewarm requires a valid main NSO header')
    # Same 16-byte build ID convention as game profiles and fixups.tsv.
    build_id = header[0x40:0x50].hex()
    path = Path(android_work) / 'pipeline-prewarm' / build_id / 'vk_pipelines.bin'
    return path if path.is_file() else None
