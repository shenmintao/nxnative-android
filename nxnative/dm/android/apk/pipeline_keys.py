"""Optional recorded Vulkan pipeline keys; driver cache blobs are never shipped."""
import struct
from pathlib import Path

ASSET = 'assets/nxnative/vk_pipelines.bin'
MAX_SIZE = 64 * 1024 * 1024


def read_keys(path):
    path = Path(path)
    if not 8 < path.stat().st_size <= MAX_SIZE:
        raise ValueError('invalid pipeline key list size')
    data = path.read_bytes()
    magic, stride = struct.unpack_from('<II', data)
    if magic != 0x31455050 or not 0 < stride <= 65536 or (len(data) - 8) % stride:
        raise ValueError('invalid or truncated PPE1 pipeline key list')
    return data
