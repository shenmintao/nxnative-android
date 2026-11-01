// Synthetic modules test provenance independently of reflection rw flags.
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static int fail_allocation;
static void *test_calloc(size_t n, size_t size) { return fail_allocation ? NULL : calloc(n,size); }
#define VK_SPV_WRITES_CALLOC test_calloc
#include "spv_buffer_writes.h"

static uint32_t w[8192]; static unsigned count;
#define OP(op, ...) do { uint32_t args[]={__VA_ARGS__}; unsigned n=sizeof args/sizeof *args; w[count++]=((n+1)<<16)|(op); memcpy(w+count,args,sizeof args);count+=n; } while(0)
static void init(void) {
    memset(w,0,sizeof w);w[0]=0x07230203;w[1]=0x10300;w[3]=1024;count=5;
    OP(32,1,12,9); // storage-buffer pointer type
    OP(32,2,7,9);  // function pointer type
    OP(32,3,0,9);  // image descriptor pointer type
    OP(71,10,33,2);OP(59,1,10,12);
    OP(71,20,33,3);OP(59,1,20,12);
    OP(71,30,33,4);OP(59,3,30,0);
    OP(71,40,33,8);OP(59,3,40,0);
    OP(59,2,50,7);
}
static const uint16_t bindings[]={2,3,4,8};
static const uint8_t pool[]={1,1,0,1};
static uint8_t out[4];
static void expect(int status, int a,int b,int c,int d) {
    assert(vk_spv_buffer_writes(w,count,bindings,pool,4,out)==status);
    assert(out[0]==a && out[1]==b && out[2]==c && out[3]==d);
}
int main(void) {
    init();OP(65,1,60,10,9);OP(61,9,61,60);OP(227,9,62,60,9,9);OP(62,50,9);
    expect(1,0,0,0,0); // loads, atomic loads and local stores do not write a pool
    init();OP(65,1,60,10,9);OP(62,60,9);expect(1,1,0,0,0);
    init();OP(66,1,60,20,9);OP(83,1,61,60);OP(124,1,62,61);OP(63,62,10);expect(1,0,1,0,0);
    init();OP(67,1,60,10,9);OP(70,1,61,60,9);OP(64,61,20,9);expect(1,1,0,0,0);
    init();OP(169,1,60,9,10,20);OP(62,60,9);expect(1,1,1,0,0);
    init();OP(245,1,60,61,90,10,91);OP(83,1,61,60);OP(62,60,9);expect(1,1,0,0,0); // cyclic phi has a rooted incoming edge
    init();OP(245,1,60,10,90,20,91);OP(62,60,9);expect(1,1,1,0,0);
    const unsigned atomics[]={229,230,231,232,233,234,235,236,237,238,239,240,241,242,318,5614,5615,6035};
    for(unsigned i=0;i<sizeof atomics/sizeof *atomics;i++) {
        init();OP(65,1,60,20,9);
        uint32_t inst[]={9,61,60,9,9,9,9,9};w[count++]=(9u<<16)|atomics[i];memcpy(w+count,inst,sizeof inst);count+=8;
        expect(1,0,1,0,0);
    }
    init();OP(228,10,9,9,9);OP(319,20,9,9);expect(1,1,1,0,0);
    init();OP(61,9,60,40);OP(99,60,9,9);expect(1,0,0,0,1); // buffer image write
    init();OP(61,9,60,40);OP(60,1,61,60,9,9);OP(234,9,62,61,9,9,9);expect(1,0,0,0,1);
    init();OP(61,9,60,30);OP(99,60,9,9);expect(1,0,0,0,0); // an ordinary image is not pool-buffer storage
    init();OP(71,70,33,2);OP(74,70,10);OP(62,10,9);expect(1,1,0,0,0);
    init();OP(55,1,60);OP(62,60,9);expect(2,1,1,0,1); // unresolved external function parameter
    init();OP(59,1,60,12);OP(62,60,9);expect(2,1,1,0,1); // missing descriptor decoration
    init();OP(71,60,33,90);OP(59,1,60,12);OP(62,60,9);expect(2,1,1,0,1); // binding absent from reflection
    init();OP(55,2,60);OP(62,60,9);expect(1,0,0,0,0); // a Function pointer cannot alias a descriptor
    init();OP(61,1,60,50);OP(62,60,9);expect(2,1,1,0,1); // loading a pointer is not descriptor provenance
    init();OP(120,1,60,9);OP(62,60,9);expect(2,1,1,0,1);
    init();OP(83,1,60,61);OP(83,1,61,60);OP(62,60,9);expect(2,1,1,0,1); // rootless cycle is conservative
    init();OP(11,80,0x4c534c47,0x6474732e,0x3035342e,0);
    OP(12,9,60,80,35,9,10);expect(1,1,0,0,0);
    init();OP(11,80,0x4c534c47,0x6474732e,0x3035342e,0);
    OP(12,9,60,80,51,9,20);expect(1,0,1,0,0);
    init();OP(259,9,60,9,9,9,9,9,9);expect(2,1,1,0,1);
    init();w[count++]=0;expect(0,1,1,0,1);
    init();w[count++]=(10u<<16)|62;expect(0,1,1,0,1);
    init();OP(71,1024,33,2);expect(0,1,1,0,1);
    init();OP(62,0,9);expect(0,1,1,0,1);
    init();w[3]=UINT32_MAX;expect(2,1,1,0,1);
    init();fail_allocation=1;expect(0,1,1,0,1);fail_allocation=0;
    init();OP(62,10,9);uint8_t none[4]={0};
    assert(vk_spv_buffer_writes(w,count,bindings,none,4,out)==1);
    for(int i=0;i<4;i++)assert(!out[i]);
    puts("PASS");return 0;
}
