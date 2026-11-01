"""Guest ring stores use the NVN offsets independently observed in the owned SDK."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NvnBufferGuestFields(unittest.TestCase):
    def test_raw_guest_stores_and_builder_independence(self):
        cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not cc:
            self.skipTest('a host C compiler is required')
        folder = ROOT / 'nxnative/dm/gfx/nvn'
        source = (folder / 'hle_nvn.c').read_text(encoding='utf8')
        types = '\n'.join(l for l in source.splitlines() if l.startswith('typedef struct {') and
                          ('} buf_builder_t;' in l or '} buf_t;' in l))
        a = source.index('NVN(nvnBufferInitialize,')
        initialize = source[a:source.index('\n}', a) + 3]
        names = ('nvnBufferBuilderSetDevice', 'nvnBufferBuilderSetDefaults', 'nvnBufferBuilderSetStorage',
                 'nvnBufferBuilderGetSize', 'nvnBufferBuilderGetMemoryOffset', 'nvnBufferGetAddress',
                 'nvnBufferGetSize', 'nvnBufferGetMemoryOffset', 'nvnBufferGetMemoryPool', 'nvnBufferMap',
                 'nvnBufferFinalize')
        hooks = '\n'.join(l for l in source.splitlines() if any(l.startswith('NVN(' + n + ',') for n in names))
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nvn_obj.h"
static void message(const char *s,...){fprintf(stderr,"%s\n",s);assert(0);}
#define LOG(...) message(__VA_ARGS__)
#define OLOG(...) ((void)0)
#define NVN(name,result,...) result name(__VA_ARGS__)
''' + types + r'''
#define BB(b) ((buf_builder_t *)(b))
#define BF(b) ((const buf_t *)(b))
''' + initialize + hooks + r'''
static uint64_t raw(const void *p,size_t off){uint64_t value;memcpy(&value,(const uint8_t *)p+off,8);return value;}
int main(void){
    static uint8_t storage[0x10000];
    pool_t pool={.mem=storage,.size=sizeof storage};NVNdevice device={0};
    const uint64_t cases[][2]={{0x100,0x200},{0x4000,0x100},{0x8000,0x300},{0,0},{0xff00,0x100}};
    for(unsigned inaccessible=0;inaccessible<2;inaccessible++){
        pool.flags=inaccessible?NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS:0;
        NVNbufferBuilder builder={0};nvnBufferBuilderSetDevice(&builder,&device);nvnBufferBuilderSetDefaults(&builder);
        nvnBufferBuilderSetStorage(&builder,(NVNmemoryPool *)&pool,0x100,0x200);
        NVNbufferBuilder before;memcpy(&before,&builder,sizeof before);
        struct{uint64_t before;NVNbuffer object;uint64_t after;}guarded={.before=UINT64_C(0x123456789abcdef0),.after=UINT64_C(0xfedcba9876543210)};
        assert(nvnBufferInitialize(&guarded.object,&builder));
        // Literal offsets come from original-driver loads/stores, not buf_t.
        assert(raw(&guarded.object,0x18)==0x100&&raw(&guarded.object,0x20)==0x200);
        for(unsigned i=0;i<sizeof cases/sizeof *cases;i++){
            memcpy((uint8_t *)&guarded.object+0x18,&cases[i][0],8);
            memcpy((uint8_t *)&guarded.object+0x20,&cases[i][1],8);
            assert(nvnBufferGetAddress(&guarded.object)==(uintptr_t)storage+cases[i][0]);
            assert(nvnBufferGetMemoryOffset(&guarded.object)==(ptrdiff_t)cases[i][0]);
            assert(nvnBufferGetSize(&guarded.object)==cases[i][1]);
            assert(nvnBufferGetMemoryPool(&guarded.object)==(NVNmemoryPool *)&pool);
            assert(nvnBufferMap(&guarded.object)==(inaccessible?NULL:storage+cases[i][0]));
            assert(!memcmp(&builder,&before,sizeof before));
            assert(nvnBufferBuilderGetMemoryOffset(&builder)==0x100&&nvnBufferBuilderGetSize(&builder)==0x200);
            assert(guarded.before==UINT64_C(0x123456789abcdef0)&&guarded.after==UINT64_C(0xfedcba9876543210));
        }
        nvnBufferFinalize(&guarded.object);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nvn-buffer-guest-') as temp:
            c, exe = Path(temp) / 'probe.c', Path(temp) / 'probe.exe'
            c.write_text(code, encoding='utf8')
            p = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(folder),
                                '-I', str(folder / 'include'), str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            p = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)


if __name__ == '__main__':
    unittest.main()
