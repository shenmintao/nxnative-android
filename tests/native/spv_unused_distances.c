#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spv_unused_distances.h"
static uint32_t w[1024], count;
#define OP(code,...) do {uint32_t args[]={__VA_ARGS__};uint32_t n=sizeof args/sizeof*args;w[count++]=((n+1)<<16)|(code);memcpy(w+count,args,sizeof args);count+=n;}while(0)
static void init(int clip_used) {
    memset(w,0,sizeof w);w[0]=0x07230203;w[1]=0x10300;w[3]=128;count=5;
    OP(43,8,9,0);OP(43,8,10,1);OP(43,8,11,2);OP(43,8,12,3);
    OP(30,41,2,1,3,4);OP(32,42,3,41);OP(32,14,3,1);OP(32,15,3,2);
    OP(72,41,0,11,0);OP(72,41,1,11,1);OP(72,41,2,11,3);OP(72,41,3,11,4);
    OP(6,41,3,0x6c6c7563,0);OP(59,42,44,3);OP(15,0,90,0x6e69616d,0,44);
    OP(54,13,90,0,16);OP(65,15,60,44,9);OP(62,60,70);
    if(clip_used){OP(65,14,61,44,11,9);OP(62,61,71);}
}
static uint32_t find(const uint32_t *words,uint32_t n,uint32_t opcode) {
    for(uint32_t at=5;at<n;at+=words[at]>>16)if((words[at]&65535)==opcode)return at;
    return n;
}
static void trimmed(unsigned members) {
    uint32_t n=0;uint32_t*out=vk_spv_trim_unused_distances(w,count,&n);assert(out&&n<count&&out[3]==w[3]);
    uint32_t type=find(out,n,30);assert((out[type]>>16)==members+2);
    uint32_t before=find(w,count,54),after=find(out,n,54);
    assert(count-before==n-after&&!memcmp(w+before,out+after,(count-before)*4));
    uint32_t second=n;assert(!vk_spv_trim_unused_distances(out,n,&second));free(out);
}
static void unchanged(void) {uint32_t n=count;assert(!vk_spv_trim_unused_distances(w,count,&n)&&n==count);}
int main(void) {
    init(1);trimmed(3); // ClipDistance stays active; unused CullDistance is removed.
    init(0);trimmed(2); // Both unused tail members can go, without moving retained indices.
    init(1);OP(12,1,80,81,44,82,83,84);trimmed(3); // An extended instruction's literal 44 is not a reference to variable 44.
    init(1);OP(17,44);OP(43,8,77,44);trimmed(3); // Capability and constant literals can also equal an output variable's ID.
    init(1);OP(65,14,62,44,12,9);OP(62,62,71);unchanged();
    init(1);OP(83,42,62,44);unchanged();
    init(1);OP(245,42,62,44,91,44,92);unchanged();
    init(1);OP(62,44,70);unchanged();
    init(1);OP(61,41,62,44);unchanged();
    init(1);OP(57,13,62,91,44);unchanged();
    init(1);OP(32,75,1,41);unchanged();
    init(1);OP(28,75,41,10);unchanged();
    init(1);OP(30,75,1,41);unchanged();
    init(1);OP(44,41,75,70,71,72,73);unchanged();
    init(1);OP(65,14,62,44,90,9);unchanged();
    init(1);OP(75,77,41,3);unchanged();
    init(1);OP(72,41,3,35,12);unchanged();
    init(1);w[count++]=0;unchanged();
    init(1);OP(32,128,3,41);unchanged();
    init(1);w[3]=UINT32_MAX;unchanged();
    puts("PASS");return 0;
}
