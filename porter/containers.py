"""Read content metadata for combined XCI/NSP containers using the real extractor."""
import hashlib
import json
import os
from pathlib import Path

from porter import config, toolchain
from porter.paths import DM, NXNATIVE
from porter.upstream import BuildError, nsp


def scan(folder, title_id=None, *, keys=None, cfg=None, env=None, selected=None):
    app, family = nsp.scan_nsps(folder, title_id=title_id)
    if selected is not None:
        selected = Path(selected)
        chosen_title = nsp.title_of(selected) if selected.is_file() else None
        if chosen_title is not None and chosen_title & 0xfff == 0:
            family['base'] = selected
    cfg = config.load() if cfg is None else cfg
    keys = keys or cfg.get('keys')
    if not keys or not Path(keys).is_file():
        return app, family
    base = family['base']
    # NSPs may also combine base/update/DLC. The CNMT, not filenames, is authoritative.
    work = folder / f'{app:016X}_build'
    logs = work / 'logs'; logs.mkdir(parents=True, exist_ok=True)
    dotnet = toolchain.find_dotnet(cfg)
    if not dotnet: raise BuildError('reading container metadata needs .NET SDK 10; run tools --download')
    run_env = dict(env or os.environ, DOTNET_ROOT=str(dotnet.parent), DOTNET_CLI_TELEMETRY_OPTOUT='1')
    run_env['PATH'] = str(dotnet.parent) + os.pathsep + run_env.get('PATH', '')
    source = DM / 'tools/nx_extract'
    code_key = hashlib.sha256(b''.join(p.read_bytes() for p in sorted(source.glob('*.cs')) + sorted(source.glob('*.csproj')))).hexdigest()
    executable = work / 'tools/nx_extract' / ('nx_extract.exe' if os.name == 'nt' else 'nx_extract')
    tool_key = work / 'tools/nx_extract.key'
    if not executable.is_file() or not tool_key.is_file() or tool_key.read_text().strip() != code_key:
        old_path = os.environ.get('PATH', '')
        try:
            os.environ['PATH'] = run_env['PATH']
            executable = nsp.build_nx_extract(NXNATIVE, work, logs, run_env)
        finally:
            os.environ['PATH'] = old_path
        tool_key.write_text(code_key, encoding='ascii')
    stat = base.stat(); key_stat = Path(keys).stat()
    cache_key = hashlib.sha256(repr((str(base), stat.st_size, stat.st_mtime_ns, str(keys), key_stat.st_size, key_stat.st_mtime_ns, code_key)).encode()).hexdigest()
    cache = work / 'container-info.json'
    try: cached = json.loads(cache.read_text(encoding='utf-8'))
    except (OSError, ValueError): cached = {}
    if cached.get('key') == cache_key:
        info = cached['info']
    else:
        log = logs / 'container_info.log'
        nsp.run([executable, keys, '--info', base], log, env=run_env)
        try: info = json.loads(log.read_text(encoding='utf-8'))
        except ValueError as exc: raise BuildError(f'cannot read container metadata: {log}') from exc
        cache.write_text(json.dumps({'key': cache_key, 'info': info}, indent=2), encoding='utf-8')
    actual = int(info['title_id'], 16)
    if actual != app: raise BuildError(f'the container has title {actual:016X}, but its filename/ticket identified {app:016X}')
    family['base_version'] = info['base_version']
    family['updates'] = [(row['version'], base) for row in info['updates']] + family['updates']
    family['dlc'] = [(int(row['title_id'], 16), row['version'], base) for row in info['dlc']] + family['dlc']
    return app, family
