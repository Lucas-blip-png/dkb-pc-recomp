"""CPU ownership regression for the native GS packed-control memo.

Extracts the actual helper and uses the real portable-control packer. Reflection
supplies original Location masks; allocator spies own all heap blocks. No shader
translation, SDK/title input, Vulkan loader or GPU execution is involved.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, declaration):
    start = source.index(declaration)
    at = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return source[start:at]


def fixture(source):
    providers = r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "nvn_geometry_control.h"
struct GfxShader { uint32_t locations; };
static struct GfxShader vertex,fragment;
static const GfxShader *native_gs_vs=&vertex,*native_gs_fs=&fragment;
static uint64_t shader_origin=17;
static struct { int amd_sc; } vk_caps={1};
uint32_t gfx_shader_inputs(const GfxShader *s,GfxShaderIo io[32]) {
    if(s!=&fragment)abort();memset(io,0,32*sizeof *io);return s->locations;
}
uint32_t gfx_shader_outputs(const GfxShader *s,GfxShaderIo io[32]) {
    if(s!=&vertex)abort();memset(io,0,32*sizeof *io);return s->locations;
}
void rt_log(const char *format,...) { (void)format; }
uint64_t nvn2_origin(const uint8_t *code) { return code?shader_origin:0; }
enum { MAX_BLOCKS=1024,CONTROL_BYTES=0x880,CODE_BYTES=0x100,CONTEXTS=257 };
static struct { void *pointer; size_t bytes; } blocks[MAX_BLOCKS];
static unsigned malloc_calls,realloc_calls,free_calls,successful_copies,live_blocks;
static int fail_malloc,fail_realloc;
static unsigned slot(void *pointer) {
    for(unsigned i=0;i<MAX_BLOCKS;i++)if(blocks[i].pointer==pointer)return i;
    abort();
}
static void *spy_malloc(size_t bytes) {
    malloc_calls++;if(fail_malloc){fail_malloc=0;return NULL;}
    if(bytes!=CONTROL_BYTES)abort();
    void *p=malloc(bytes);if(!p)abort();unsigned i=slot(NULL);
    blocks[i].pointer=p;blocks[i].bytes=bytes;live_blocks++;successful_copies++;
    return p;
}
static void *spy_realloc(void *pointer,size_t bytes) {
    realloc_calls++;if(fail_realloc){fail_realloc=0;return NULL;}
    if(!bytes||bytes>65536)abort();
    int was_null=pointer==NULL;unsigned i=slot(pointer);
    void *p=realloc(pointer,bytes);if(!p)abort();
    if(was_null)live_blocks++;
    blocks[i].pointer=p;blocks[i].bytes=bytes;return p;
}
static void spy_free(void *pointer) {
    if(!pointer)abort();unsigned i=slot(pointer);
    if(blocks[i].bytes!=CONTROL_BYTES)abort();
    free_calls++;live_blocks--;blocks[i].pointer=NULL;blocks[i].bytes=0;free(pointer);
}
#define malloc spy_malloc
#define realloc spy_realloc
#define free spy_free
'''
    cases = r'''
#undef malloc
#undef realloc
#undef free
static uint8_t controls[CONTEXTS][CONTROL_BYTES],original[sizeof controls],code[CODE_BYTES];
static const uint8_t *copies[CONTEXTS];
static unsigned checks,failures;
#define CHECK(c) do {checks++;if(!(c)){failures++;fprintf(stderr,"%s:%d: %s\n",name,__LINE__,#c);}}while(0)
static void setup(void) {
    for(unsigned i=0;i<CONTEXTS;i++) {
        memset(controls[i],0,sizeof controls[i]);
        nvn_geometry_control_put(controls[i],i+1);
        nvn_geometry_control_put(controls[i]+0x700,CODE_BYTES);
        nvn_geometry_control_put(controls[i]+0x704,CODE_BYTES);
        nvn_geometry_control_put(controls[i]+0x714,2);
    }
    memcpy(original,controls,sizeof controls);
    nvn_geometry_control_put(code+0x30,1u|(3u<<5)|(4u<<10)|(1u<<24));
    nvn_geometry_control_put(code+0x38,1u<<24);nvn_geometry_control_put(code+0x3c,7u<<24);
    nvn_geometry_control_put(code+0x40,1);
    vertex.locations=0x80000115u;fragment.locations=0x80000217u;
}
static void verify(const uint8_t *copy,unsigned context,uint32_t mask,const char *name) {
    CHECK(copy!=NULL);if(!copy)return;
    CHECK(copy!=controls[context]);CHECK(blocks[slot((void *)copy)].bytes==CONTROL_BYTES);
    CHECK(!memcmp(copy,controls[context],NVN_GEOMETRY_CONTROL_OFFSET));
    CHECK(!memcmp(copy+0x840,controls[context]+0x840,CONTROL_BYTES-0x840));
    uint32_t skip[8];CHECK(nvn_geometry_control_get(copy,CONTROL_BYTES,code,CODE_BYTES,skip)==1);
    CHECK(skip[0]==0x0fffffffu);
    for(unsigned l=0;l<32;l++)CHECK((skip[1+l/8]>>(l%8*4)&15)==((mask>>l&1)?0:15));
    CHECK(skip[5]==UINT32_MAX&&skip[6]==UINT32_MAX&&skip[7]==UINT32_MAX);
    CHECK(!memcmp(original,controls,sizeof controls));
}
static void memo(const char *name) {
    for(unsigned i=0;i<CONTEXTS;i++) {
        copies[i]=native_gs_control(controls[i],code,CODE_BYTES);
        verify(copies[i],i,0x80000015u,name);
        if(i)CHECK(copies[i]!=copies[i-1]);
    }
    CHECK(successful_copies==CONTEXTS);CHECK(free_calls==0);
    unsigned allocations=malloc_calls,growth=realloc_calls;
    for(unsigned i=0;i<CONTEXTS;i++)CHECK(native_gs_control(controls[i],code,CODE_BYTES)==copies[i]);
    for(unsigned i=0;i<8;i++)CHECK(native_gs_control(controls[256],code,CODE_BYTES)==copies[256]);
    CHECK(malloc_calls==allocations);CHECK(realloc_calls==growth);
    vertex.locations=0xff;fragment.locations=2;
    const uint8_t *other=native_gs_control(controls[256],code,CODE_BYTES);
    verify(other,256,2,name);CHECK(other!=copies[256]);
    CHECK(native_gs_control(controls[256],code,CODE_BYTES)==other);
    vertex.locations=0x80000115u;fragment.locations=0x80000217u;
    CHECK(native_gs_control(controls[256],code,CODE_BYTES)==copies[256]);
    vertex.locations=0;fragment.locations=UINT32_MAX;
    const uint8_t *empty=native_gs_control(controls[256],code,CODE_BYTES);
    verify(empty,256,0,name);CHECK(empty!=other&&empty!=copies[256]);
    CHECK(native_gs_control(controls[256],code,CODE_BYTES)==empty);
    // Different reflected sets with the same intersection reuse the same context.
    vertex.locations=2;fragment.locations=UINT32_MAX;
    allocations=malloc_calls;CHECK(native_gs_control(controls[256],code,CODE_BYTES)==other);
    CHECK(malloc_calls==allocations);CHECK(free_calls==0);CHECK(successful_copies==259);
    CHECK(live_blocks==260); // 259 stable controls plus one process-owned memo allocation.
    verify(copies[0],0,0x80000015u,name);verify(copies[256],256,0x80000015u,name);
}
static void allocation_failure(const char *name,int growth) {
    if(growth)for(unsigned i=0;i<256;i++)copies[i]=native_gs_control(controls[i],code,CODE_BYTES);
    unsigned allocations=malloc_calls,frees=free_calls,live=live_blocks;
    if(!strcmp(name,"copy-failure"))fail_malloc=1;else fail_realloc=1;
    unsigned index=growth?256:0;
    CHECK(native_gs_control(controls[index],code,CODE_BYTES)==NULL);
    CHECK(malloc_calls==allocations+1);
    CHECK(free_calls==frees+(!strcmp(name,"copy-failure")?0:1));
    CHECK(live_blocks==live);CHECK(!memcmp(original,controls,sizeof controls));
    if(growth)for(unsigned i=0;i<256;i++)CHECK(native_gs_control(controls[i],code,CODE_BYTES)==copies[i]);
    const uint8_t *retry=native_gs_control(controls[index],code,CODE_BYTES);
    verify(retry,index,0x80000015u,name);
    allocations=malloc_calls;unsigned resizes=realloc_calls;
    CHECK(native_gs_control(controls[index],code,CODE_BYTES)==retry);
    CHECK(malloc_calls==allocations);CHECK(realloc_calls==resizes);
    if(growth)verify(copies[0],0,0x80000015u,name);
}
static void packing_failure(const char *name) {
    controls[0][0x800]=0x7f;memcpy(original,controls,sizeof controls);
    CHECK(native_gs_control(controls[0],code,CODE_BYTES)==NULL);
    CHECK(malloc_calls==1&&free_calls==1&&realloc_calls==0&&live_blocks==0);
    CHECK(!memcmp(original,controls,sizeof controls));
    const uint8_t *valid=native_gs_control(controls[1],code,CODE_BYTES);
    verify(valid,1,0x80000015u,name);CHECK(successful_copies==2&&free_calls==1);
}
static void voxel_mask(const char *name) {
    shader_origin=UINT64_C(0x3dbaaf649cb5f927);
    vertex.locations=UINT32_C(0x00080001);fragment.locations=0;
    const uint8_t *copy=native_gs_control(controls[0],code,CODE_BYTES);
    verify(copy,0,UINT32_C(0x00080001),name);
    CHECK(successful_copies==1&&free_calls==0&&live_blocks==2);
    CHECK(native_gs_control(controls[0],code,CODE_BYTES)==copy);
    vk_caps.amd_sc=0;
    const uint8_t *other=native_gs_control(controls[0],code,CODE_BYTES);
    verify(other,0,0,name);CHECK(other!=copy);
    CHECK(successful_copies==2&&free_calls==0&&live_blocks==3);
    CHECK(native_gs_control(controls[0],code,CODE_BYTES)==other);
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;const char *name=argv[1];setup();(void)spy_realloc;
    if(!strcmp(name,"memo"))memo(name);
    else if(!strcmp(name,"copy-failure")||!strcmp(name,"initial-growth-failure")||!strcmp(name,"growth-failure"))
        allocation_failure(name,!strcmp(name,"growth-failure"));
    else if(!strcmp(name,"packing-failure"))packing_failure(name);
    else if(!strcmp(name,"voxel-mask"))voxel_mask(name);else return 2;
    printf("%s: %u checks, %u failures; malloc=%u realloc=%u free=%u live=%u\n",name,checks,failures,
           malloc_calls,realloc_calls,free_calls,live_blocks);
    // Production retains these controls until process exit. Only this fixture owns teardown.
    for(unsigned i=0;i<MAX_BLOCKS;i++)free(blocks[i].pointer);
    return failures?1:0;
}
'''
    return providers + function(source, 'static const uint8_t *native_gs_control(') + cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--deps', type=Path, required=True)
    parser.add_argument('--opt', choices=('0', '2'), required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--draw-source', type=Path, help='optional exact saved PR67 baseline source')
    args = parser.parse_args()
    out = args.out.resolve();out.relative_to((ROOT / 'build').resolve());out.mkdir(parents=True, exist_ok=False)
    source = args.draw_source or ROOT / 'runtime/vk_exec_draw.c'
    path = out / 'fixture.c';path.write_text(fixture(source.read_text()))
    exe = out / 'fixture.exe'
    command = [args.cc, '-std=gnu11', '-O'+args.opt, '-Wall', '-Wextra', '-Werror', '-I', ROOT / 'runtime',
               '-I', args.deps / 'include', path, '-o', exe]
    compiled = subprocess.run([str(v) for v in command], capture_output=True, text=True, timeout=60)
    (out / 'compile.log').write_text(compiled.stdout+compiled.stderr)
    if compiled.returncode:raise RuntimeError(compiled.stderr)
    rows=[]
    for case in ('memo', 'copy-failure', 'initial-growth-failure', 'growth-failure', 'packing-failure', 'voxel-mask'):
        result=subprocess.run([str(exe),case],env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1'),
                              capture_output=True,text=True,timeout=30)
        (out/f'{case}.log').write_text(result.stdout+result.stderr)
        rows.append(dict(case=case,exit=result.returncode,result=result.stdout.strip()))
        print(result.stdout,end='')
        if result.returncode:print(result.stderr,end='')
    report={'results':rows,'optimization':args.opt,'draw_source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
            'fixture_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
            'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'providers':['actual extracted native_gs_control and actual portable pack/get helpers; actual gfx.h types',
                         'original owned immutable control/header bytes and original reflected Location-mask scalars',
                         'bounded allocator spies with failure injection and fixture-only process-exit teardown',
                         'origin/log metadata only; no native lowering, shader translation or execution'],
            'limits':['same replay-thread serialization as production; no concurrent-call claim',
                      'normal malloc/realloc failures tested, SIZE_MAX overflow branches reviewed but not executed',
                      'no Vulkan/GPU or title/SDK inputs']}
    (out/'handoff.json').write_text(json.dumps(report,indent=2)+'\n')
    return int(any(r['exit'] for r in rows))


if __name__=='__main__':raise SystemExit(main())
