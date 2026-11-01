"""Buffer initialization follows original NVN observations, independent of pool size."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NvnBufferContract(unittest.TestCase):
    def test_logical_ranges_physical_pools_failure_state_and_mapping(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        folder = ROOT / 'nxnative/dm/gfx/nvn'
        source = (folder / 'hle_nvn.c').read_text(encoding='utf-8')
        types = '\n'.join(line for line in source.splitlines() if line.startswith('typedef struct {') and
                          ('} buf_builder_t;' in line or '} buf_t;' in line))
        start = source.index('NVN(nvnBufferInitialize,')
        initialize = source[start:source.index('\n}', start) + 3]
        names = ('nvnBufferBuilderSetStorage', 'nvnBufferBuilderGetSize', 'nvnBufferBuilderGetMemoryOffset',
                 'nvnBufferGetAddress', 'nvnBufferMap', 'nvnBufferGetSize', 'nvnBufferGetMemoryPool', 'nvnBufferGetMemoryOffset')
        hooks = '\n'.join(line for line in source.splitlines() if any(line.startswith('NVN(' + name + ',') for name in names))
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nvn_obj.h"
static unsigned errors;
static void message(const char *format, ...) { assert(strstr(format,"[RED]")); errors++; }
#define LOG(...) message(__VA_ARGS__)
#define OLOG(...) ((void)0)
#define NVN(name, result, ...) result name(__VA_ARGS__)
''' + types + r'''
#define BB(b) ((buf_builder_t *)(b))
#define BF(b) ((const buf_t *)(b))
''' + initialize + '\n' + hooks + r'''
static void guards(const uint8_t *p, size_t n) { for(size_t i=0;i<n;i++) assert(p[i]==0xa5); }
int main(void) {
    static uint8_t storage[0x40100];
    pool_t pool={.mem=storage,.size=0x24000};
    NVNbufferBuilder builder;
    struct { uint8_t before[16]; NVNbuffer object; uint8_t after[16]; } guarded;
    const struct { int64_t offset; uint64_t length; } cases[]={
        {0,0}, {0,1}, {0,0x24000}, {0,0x40000}, {0x100,0x23f00}, {0x23f80,0x100}
    };
    // The original owned SDK's BufferInitialize accepts logical spans beyond a
    // pool and rejects PHYSICAL pools; it clears its object on that failure.
    for(unsigned flags=0;flags<1024;flags++) for(unsigned i=0;i<sizeof cases/sizeof *cases;i++) {
        memset(&builder,0,sizeof builder); memset(&guarded,0xa5,sizeof guarded); pool.flags=flags;
        nvnBufferBuilderSetStorage(&builder,(NVNmemoryPool *)&pool,cases[i].offset,cases[i].length);
        NVNbufferBuilder snapshot; memcpy(&snapshot,&builder,sizeof snapshot);
        assert(nvnBufferBuilderGetSize(&builder)==cases[i].length);
        assert(nvnBufferBuilderGetMemoryOffset(&builder)==cases[i].offset);
        int expected=(flags&NVN_MEMORY_POOL_FLAGS_PHYSICAL)==0;
        assert((nvnBufferInitialize(&guarded.object,&builder)&0xff)==expected);
        assert(!errors && !memcmp(&snapshot,&builder,sizeof snapshot));
        guards(guarded.before,sizeof guarded.before); guards(guarded.after,sizeof guarded.after);
        if(!expected) {
            const uint8_t *bytes=(const uint8_t *)&guarded.object;
            for(size_t j=0;j<sizeof(buf_t);j++) assert(bytes[j]==0);
            continue;
        }
        assert(nvnBufferGetSize(&guarded.object)==cases[i].length);
        assert(nvnBufferGetMemoryOffset(&guarded.object)==cases[i].offset);
        assert(nvnBufferGetMemoryPool(&guarded.object)==(NVNmemoryPool *)&pool);
        assert(nvnBufferGetAddress(&guarded.object)==(uintptr_t)(storage+cases[i].offset));
        assert(nvnBufferMap(&guarded.object)==(flags&NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS ? NULL : storage+cases[i].offset));
        assert(pool.mem==storage && pool.size==0x24000 && pool.flags==flags);
    }
    for(unsigned missing=0;missing<2;missing++) {
        memset(&guarded,0xa5,sizeof guarded); pool.flags=0; pool.mem=NULL;
        nvnBufferBuilderSetStorage(&builder,missing ? NULL : (NVNmemoryPool *)&pool,0,64);
        assert(!nvnBufferInitialize(&guarded.object,&builder));
        const uint8_t *bytes=(const uint8_t *)&guarded.object;
        for(size_t j=0;j<sizeof(buf_t);j++) assert(!bytes[j]);
        guards(guarded.before,sizeof guarded.before); guards(guarded.after,sizeof guarded.after);
    }
    assert(errors==2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nvn-buffer-contract-') as temp:
            c, exe = Path(temp) / 'buffer.c', Path(temp) / 'buffer-check'
            c.write_text(code, encoding='utf-8')
            result = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(folder), '-I', str(folder / 'include'), str(c), '-o', str(exe)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
