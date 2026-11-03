"""Build a tiny desktop decoder with the NDK already required by the converter."""
import hashlib
import os
import platform
import sys
from pathlib import Path

from porter import paths
from porter.upstream import nsp


def yaz0(ndk, logs, env):
    if sys.platform not in ('win32', 'linux') or platform.machine().lower() not in ('amd64', 'x86_64'):
        return None
    windows = os.name == 'nt'
    host = 'windows-x86_64' if windows else 'linux-x86_64'
    tools = Path(ndk) / 'toolchains/llvm/prebuilt' / host / 'bin'
    compiler = tools / ('clang.exe' if windows else 'clang')
    linker = tools / ('ld.lld.exe' if windows else 'ld.lld')
    source = paths.DM / 'gfx/tools/yaz0_native.c'
    target = 'x86_64-pc-windows-msvc' if windows else 'x86_64-linux-gnu'
    flags = ['--target=' + target, '-O3', '-ffreestanding', '-fno-builtin', '-fno-stack-protector']
    if not windows: flags.append('-fPIC')
    key = hashlib.sha256(source.read_bytes() + repr((str(compiler), flags)).encode()).hexdigest()[:16]
    out = paths.user_dir() / 'tools/native'; out.mkdir(parents=True, exist_ok=True)
    library = out / ('yaz0-' + key + ('.dll' if windows else '.so'))
    if library.is_file(): return library
    obj = out / ('yaz0-' + key + ('.obj' if windows else '.o'))
    nsp.run([compiler, *flags, '-c', source, '-o', obj], logs / 'yaz0_compile.log', env=env)
    link_flags = ['-flavor', 'link', '/dll', '/noentry', '/nodefaultlib', '/out:' + str(library)] if windows else ['-shared', '-o', library]
    nsp.run([linker, *link_flags, obj], logs / 'yaz0_link.log', env=env)
    return library


def textures(ndk, logs, env):
    """Desktop BC helpers, built by the NDK host compiler with no SDK/CRT dependency."""
    windows = os.name == 'nt'
    if sys.platform not in ('win32', 'linux') or platform.machine().lower() not in ('amd64', 'x86_64'):
        raise RuntimeError('offline ASTC generation currently needs a Windows or Linux x64 host')
    tools = Path(ndk) / 'toolchains/llvm/prebuilt' / ('windows-x86_64' if windows else 'linux-x86_64') / 'bin'
    src = paths.REPO / 'porter/native'
    files = [src / 'texture_codec.c', paths.DM / 'gfx/nvn/nvn_tex.c', paths.DM / 'gfx/tex/nvn_deswizzle.c']
    inputs = files + [src / 'bc_astc.h', src / 'texture_include/string.h', paths.DM / 'third_party/bcdec/bcdec.h',
                      paths.DM / 'gfx/nvn/nvn_tex.h', paths.DM / 'gfx/tex/nvn_deswizzle.h']
    flags = ['--target=' + ('x86_64-pc-windows-msvc' if windows else 'x86_64-linux-gnu'), '-O3', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
             '-I' + str(src / 'texture_include'), '-I' + str(paths.DM / 'gfx/nvn')]
    if not windows: flags += ['-fPIC', '-fvisibility=hidden']
    key = hashlib.sha256(b''.join(p.read_bytes() for p in inputs) + repr((str(tools), flags)).encode()).hexdigest()[:16]
    out = paths.user_dir() / 'tools/native'; out.mkdir(parents=True, exist_ok=True)
    lib = out / ('textures-' + key + ('.dll' if windows else '.so'))
    if lib.is_file(): return lib
    objects = []
    for i, f in enumerate(files):
        obj = out / f'textures-{key}-{i}.o'; objects.append(obj)
        nsp.run([tools / ('clang.exe' if windows else 'clang'), *flags, '-c', f, '-o', obj], logs / f'textures_compile_{i}.log', env=env)
    link = ['-flavor', 'link', '/dll', '/noentry', '/nodefaultlib', '/out:' + str(lib)] if windows else ['-shared', '-o', lib]
    nsp.run([tools / ('ld.lld.exe' if windows else 'ld.lld'), *link, *objects], logs / 'textures_link.log', env=env)
    return lib
