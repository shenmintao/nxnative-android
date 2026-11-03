"""Offline validation of the production base shader path and depth rewrite. No device calls."""
from pathlib import Path
import argparse, concurrent.futures, hashlib, json, os, shutil, subprocess, sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from porter.toolchain import find_ndk

def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 3]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bundle', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--jobs', type=int, default=6)
    a = p.parse_args(); a.out.mkdir(parents=True, exist_ok=True)
    ndk, _ = find_ndk(); assert ndk
    cc = shutil.which('cc') or shutil.which('clang'); assert cc
    headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'))
    validator = next(ndk.glob('shader-tools/*/spirv-val*'))
    android = ROOT / 'nxnative/dm/android'; source = (android / 'gfx_vk_shader.c').read_text(encoding='utf8')
    header = (android / 'gfx_vk.h').read_text(encoding='utf8'); start = header.index('enum { RK_')
    types = header[start:header.index('} vkshader_t;', start) + len('} vkshader_t;')]
    base = (ROOT / 'tests/native/spv_nonwritable.c').read_text(encoding='utf8').split('/* SHADER FUNCTIONS */')[0]
    base = base.replace('/* SHADER TYPES */', types)
    for name, include in [('vk_spv_sampler_use', 'spv_sampler_use.h'), ('vk_spv_uniform_extents', 'spv_uniform_extent.h'),
                          ('vk_spv_vertex_outputs', 'spv_vertex_outputs.h'), ('vk_spv_trim_unused_distances', 'spv_unused_distances.h')]:
        line = next(x for x in base.splitlines() if (' ' + name + '(') in x or ('*' + name + '(') in x)
        base = base.replace(line, '#include "' + include + '"')
    base = base.replace('static uint32_t captured[4096], captured_n;', 'static uint32_t *captured, captured_n;')
    base = base.replace('assert(captured_n <= 4096);', 'captured = realloc(captured, ci->codeSize); assert(captured);')
    base = base.replace('static uint8_t bundle[32768]; static const uint8_t *g_bundle = bundle;', 'static uint8_t *bundle; static const uint8_t *g_bundle;')
    functions = '\n'.join(function(source, s) for s in ('static void shader_nonwritable(', 'vkshader_t *vk_shader('))
    code = base + functions + r"""
+static void release_shader(vkshader_t*s){free(s->var);free(s->spv);free(s);g_shaders=NULL;}
+int main(int argc,char**argv){
+ if(argc!=3)return 2;FILE*f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long bytes=ftell(f);rewind(f);
+ bundle=malloc(bytes);assert(fread(bundle,1,bytes,f)==(size_t)bytes);fclose(f);g_bundle=bundle;assert(!memcmp(bundle,"DMSV",4));
+ uint32_t n;memcpy(&n,bundle+8,4);V.has_xfb=1;V.props.limits.maxDescriptorSetUniformBuffersDynamic=16;
+ for(uint32_t i=0;i<n;i++){
+  memcpy(&entry,bundle+12+i*32,32);V.vertex_stores=1;vkshader_t*s=vk_shader(entry.key,entry.stage);assert(s);
+  uint32_t *before=malloc(s->nspv*4);memcpy(before,s->spv,s->nspv*4);uint32_t*out=NULL,nw=s->nspv;vk_spv_depth_info info;
+  assert(vk_spv_depth_clip(s->spv,s->nspv,0,entry.stage,&out,&nw,&info)==0&&!out&&nw==s->nspv&&!memcmp(before,s->spv,nw*4));free(before);
+  int status=0;unsigned captured_positions=0;
+  if(entry.stage==0||entry.stage==2){
+   release_shader(s);V.vertex_stores=0;s=vk_shader(entry.key,entry.stage);assert(s);nw=s->nspv;
+   status=vk_spv_depth_clip(s->spv,s->nspv,1,entry.stage,&out,&nw,&info);captured_positions=info.captured_positions;
+   if(status==1){char path[2048];snprintf(path,sizeof path,"%s/%016llx-%u.spv",argv[2],(unsigned long long)entry.key,entry.stage);
+    f=fopen(path,"wb");assert(f&&fwrite(out,4,nw,f)==nw);fclose(f);}
+  }
+  printf("%016llx\t%u\t%d\t%u\t%u\n",(unsigned long long)entry.key,entry.stage,status,nw,captured_positions);free(out);release_shader(s);
+ }
+ free(captured);free(bundle);return 0;
+}
+""".replace('\n+', '\n')
    c, exe = a.out / 'runtime-depth.c', a.out / 'runtime-depth.exe'
    c.write_text(code, encoding='utf8')
    command = [cc, '-std=gnu11', '-O2', '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-misleading-indentation',
               '-idirafter', str(headers), '-I' + str(android), str(c), '-pthread', '-o', str(exe)]
    subprocess.run(command, check=True)
    with (a.out / 'native-census.tsv').open('w', encoding='utf8') as log:
        subprocess.run([str(exe), str(a.bundle.resolve()), str(a.out.resolve())], stdout=log, check=True)
    rows = []
    for line in (a.out / 'native-census.tsv').read_text().splitlines():
        key, stage, status, words, captures = line.split('\t')
        row = dict(key=key, stage=int(stage), patch=int(status), words=int(words), captured_positions=int(captures),
                   extension_present_byte_identical=True)
        rows.append(row)
    def check(row):
        if row['patch'] != 1:
            return {**row, 'validation': 'not_patched' if row['patch'] == 0 else 'PATCH_FAILED'}
        path = a.out / (row['key'] + '-' + str(row['stage']) + '.spv')
        checked = subprocess.run([str(validator), '--target-env', 'vulkan1.3', str(path)], capture_output=True, text=True)
        return {**row, 'validation': 'PASS' if checked.returncode == 0 else 'FAIL', 'error': checked.stdout + checked.stderr,
                'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
    results=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for i, row in enumerate(pool.map(check, rows), 1):
            results.append(row)
            if i % 1000 == 0: print('VALIDATED', i, '/', len(rows), flush=True)
    report = dict(bundle=str(a.bundle), bundle_sha256=hashlib.sha256(a.bundle.read_bytes()).hexdigest(),
                  target_env='vulkan1.3 (runtime requested API); no relaxed validator flags', shaders=len(rows),
                  patched=sum(r['patch']==1 for r in results), captures=sum(r['captured_positions'] for r in results),
                  gate_byte_identical=len(rows), failures=sum(r['validation'] in ['FAIL','PATCH_FAILED'] for r in results), rows=results)
    (a.out / 'results.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf8')
    print(json.dumps({k:v for k,v in report.items() if k!='rows'}), flush=True)
    if report['failures']: raise SystemExit(1)

if __name__ == '__main__': main()
