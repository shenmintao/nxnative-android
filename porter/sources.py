"""Resolve a game folder or a selected NSP/XCI without copying game files."""
import struct
from pathlib import Path

from porter.upstream import BuildError, nsp


def resolve(source):
    path = Path(source).expanduser().resolve()
    if path.is_dir():
        return path, None
    if not path.is_file():
        raise BuildError(f'no such game file or folder: {path}')
    if path.suffix.lower() not in ('.nsp', '.xci'):
        raise BuildError('choose an NSP or XCI file, or a folder containing the base game, updates and DLC')
    try:
        if path.suffix.lower() == '.nsp':
            with path.open('rb') as stream:
                header = stream.read(16)
            if len(header) != 16 or header[:4] != b'PFS0':
                raise BuildError(f'not an NSP/PFS0 container: {path.name}')
            count, strings = struct.unpack_from('<II', header, 4)
            if 16 + count * 24 + strings > path.stat().st_size:
                raise BuildError(f'incomplete NSP file table: {path.name}')
        title_id = nsp.title_of(path)
    except (OSError, ValueError, struct.error) as exc:
        raise BuildError(f'cannot read game input {path.name}: {exc}') from exc
    if title_id is None:
        raise BuildError(f'cannot identify {path.name}: keep the [16-digit title ID] in its filename when it has no ticket')
    return path.parent, title_id
