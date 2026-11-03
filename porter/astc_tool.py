"""Pinned Arm astcenc CLI, fetched only for a non-BC port (Apache-2.0)."""
import hashlib
import os
import platform
import stat
import sys
import urllib.request
import zipfile
from pathlib import Path

from porter import paths
from porter.upstream import BuildError, PROG

VERSION = '5.7.0'
COMMIT = 'baff485b0ff36d2f95d28961605106502c653966'
ARCHIVES = {
    'windows-x64': 'cbde7c78adcc90d0d90ecf10f32d03f3340fbe1720cb6d6c892147d625e74d2d',
    'windows-arm64': '5b18285fda1eeb884fff5d1846c5669bafd38552c55c6a08d5bd8f6b8589c081',
    'linux-x64': '6c6b299b17608e1ee74db9c491cdc793b12b2b104a4b52222e358d78ae0662a9',
    'linux-arm64': '43f2544127f40340768e7cac317df74101a0b0e30619fe68c6e2628ceaa91692',
    'macos-universal': '374b2f0aea1d3ab5f849784666d6ece6e27b4bc1f945e5c90d6eeabf00e955f8',
}


def archive_name():
    system = {'win32': 'windows', 'darwin': 'macos'}.get(sys.platform, 'linux')
    cpu = 'arm64' if platform.machine().lower() in ('aarch64', 'arm64') else 'x64'
    return f'astcenc-{VERSION}-{system}-' + ('universal' if system == 'macos' else cpu) + '.zip'


def get(avx2=False):
    name = archive_name(); host = name.removeprefix(f'astcenc-{VERSION}-').removesuffix('.zip')
    root = paths.player_tools() / f'astcenc-{VERSION}-{host}'
    arc = root / name; expected = ARCHIVES[host]
    root.mkdir(parents=True, exist_ok=True)
    if not arc.is_file() or hashlib.sha256(arc.read_bytes()).hexdigest() != expected:
        url = f'https://github.com/ARM-software/astc-encoder/releases/download/{VERSION}/{name}'
        PROG.set(step=f'downloading Arm astcenc {VERSION} (Apache-2.0)')
        part = arc.with_suffix('.part')
        try:
            with urllib.request.urlopen(url, timeout=120) as src, part.open('wb') as dst:
                while chunk := src.read(1 << 20): dst.write(chunk)
            if hashlib.sha256(part.read_bytes()).hexdigest() != expected: raise BuildError('astcenc download checksum mismatch')
            part.replace(arc)
        finally: part.unlink(missing_ok=True)
    # Extract only the portable baseline ISA and its notices. Verify on every use.
    suffix = '.exe' if os.name == 'nt' else ''
    isa = ('avx2' if avx2 else 'sse2') if host.endswith('x64') else 'neon'
    wanted = f'astcenc-{isa}{suffix}'
    with zipfile.ZipFile(arc) as z:
        candidates = [n for n in z.namelist() if Path(n).name == wanted]
        if not candidates: raise BuildError(f'astcenc archive lacks {wanted}')
        for n in z.namelist():
            leaf = Path(n).name
            if n == candidates[0] or any(k in leaf.lower() for k in ('license', 'notice')):
                (root / leaf).write_bytes(z.read(n))
    executable = root / wanted
    if os.name != 'nt': executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
    for leaf in ('LICENSE.txt', 'NOTICE.txt'):
        notice = paths.DM / 'third_party/astcenc' / leaf
        if notice.is_file(): (root / leaf).write_bytes(notice.read_bytes())
    return executable
