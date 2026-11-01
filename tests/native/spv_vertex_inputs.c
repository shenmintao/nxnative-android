#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static unsigned allocation, fail_allocation;
static void *checked_malloc(size_t n) { return ++allocation == fail_allocation ? NULL : malloc(n); }
static void *checked_calloc(size_t n, size_t s) { return ++allocation == fail_allocation ? NULL : calloc(n, s); }
#define malloc checked_malloc
#define calloc checked_calloc
#include "spv_vertex_inputs.h"
#undef malloc
#undef calloc

static uint32_t words[2048], count;
#define OP(code,...) do { uint32_t a[]={__VA_ARGS__}; words[count++]=((uint32_t)(sizeof a/sizeof*a)+1)<<16|(code); memcpy(words+count,a,sizeof a); count+=(uint32_t)(sizeof a/sizeof*a); } while(0)
static void init(unsigned kind) {
    memset(words,0,sizeof words); words[0]=0x07230203;words[1]=0x10300;words[3]=128;count=5;
    OP(17,1);OP(14,0,1);OP(15,0,51,0x6e69616d,0,20,30);OP(71,20,30,0);OP(71,30,30,1);
    OP(22,1,32);OP(21,2,32,0);OP(21,3,32,1);
    for(unsigned k=1;k<=3;k++){OP(23,3+k,k,4);OP(32,9+k,1,3+k);OP(32,12+k,1,k);}
    for(unsigned j=0;j<4;j++)OP(43,2,41+j,j);
    OP(59,9+kind,20,1);OP(59,9+kind,30,1);OP(54,1,51,0,50);OP(248,52);
    for(unsigned j=0;j<4;j++){OP(65,12+kind,70+j,20,41+j);OP(61,kind,60+j,70+j);OP(124,2,80+j,60+j);}
    OP(253,0); // A semantic instruction evaluator, not the validator's complete-module fixture.
    OP(56,0);
}
static uint32_t float_bits(float f) {uint32_t b;memcpy(&b,&f,4);return b;}
// Evaluate the register path independently of the rewriter: memory contains
// the fetch result, bitcasts retain bits, conversions produce numeric floats.
static void evaluate(const uint32_t *w,uint32_t n,unsigned fetched,const uint32_t input[4],const uint32_t expected[4]) {
    uint32_t defs[1024]={0},values[1024]={0},element[1024]={0},scalar[1024]={0};
    assert(w[3]<1024);
    for(uint32_t at=5;at<n;at+=w[at]>>16){const uint32_t*x=w+at;unsigned op=x[0]&65535;
        if(op==21||op==22||op==23||op==32)defs[x[1]]=at;
        if(op==21)scalar[x[1]]=x[3]?3:2;
        if(op==22)scalar[x[1]]=1;
        if(op==23)scalar[x[1]]=scalar[x[2]];
        if(op==43)values[x[2]]=x[3];
        if(op==59&&x[2]==20){const uint32_t*p=w+defs[x[1]];assert(scalar[p[3]]==fetched);}
        if(op==65&&x[3]==20)element[x[2]]=values[x[4]];
        if(op==61){assert(element[x[3]]<4);values[x[2]]=input[element[x[3]]];}
        if(op==124)values[x[2]]=values[x[3]];
        if(op==111)values[x[2]]=float_bits((float)(int32_t)values[x[3]]);
        if(op==112)values[x[2]]=float_bits((float)values[x[3]]);
    }
    for(unsigned j=0;j<4;j++)assert(values[80+j]==expected[j]);
}
static void semantic(unsigned declared,unsigned code,const uint32_t raw[4],const uint32_t expected[4]) {
    init(declared);uint32_t source[2048];memcpy(source,words,sizeof words);uint32_t*out=NULL,n=count;
    int r=vk_spv_vertex_inputs(words,count,code,&out,&n);assert(r==1||r==0);assert(!memcmp(source,words,sizeof words));
    evaluate(out?out:words,n,code==4?2:code==5?3:code,raw,expected);free(out);
}
static int file_rewrite(char**argv) {
    FILE*f=fopen(argv[1],"rb");if(!f)return 2;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
    if(n<20||(n&3)){fclose(f);return 2;}uint32_t*w=malloc((size_t)n),*out=NULL,nw=0;
    if(!w||fread(w,1,(size_t)n,f)!=(size_t)n){fclose(f);free(w);return 2;}fclose(f);
    int r=vk_spv_vertex_inputs(w,(uint32_t)n/4,strtoull(argv[3],NULL,16),&out,&nw);if(r<0){free(w);return 3;}
    f=fopen(argv[2],"wb");if(!f){free(out);free(w);return 2;}fwrite(out?out:w,4,nw,f);fclose(f);free(out);free(w);return 0;
}
int main(int argc,char**argv) {
    if(argc==4)return file_rewrite(argv);
    assert(argc==1);
    const uint32_t floats[4]={0x3f800000,0xbf800000,0,0x3f000000};
    const uint32_t integers[4]={255,65535,0x80000000,1};
    const uint32_t signed_values[4]={0xffffffff,0xffffff80,0x80000000,0x7fffffff};
    const uint32_t scaled[4]={1,200,65535,16777216},scaled_expected[4]={0x3f800000,0x43480000,0x477fff00,0x4b800000};
    const uint32_t signed_scaled[4]={0xffffffff,0xffffff80,0xffff0001,0},signed_expected[4]={0xbf800000,0xc3000000,0xc77fff00,0};
    for(unsigned declared=1;declared<=3;declared++){
        semantic(declared,1,floats,floats);semantic(declared,2,integers,integers);semantic(declared,3,signed_values,signed_values);
        semantic(declared,4,scaled,scaled_expected);semantic(declared,5,signed_scaled,signed_expected);
    }
    uint32_t*out=NULL,n;init(1);assert(vk_spv_vertex_inputs(words,count,0,&out,&n)==0&&!out&&n==count);
    // A pointer copy, select and cyclic phi keep their fetched class. Mixing a
    // changed pointer with an unchanged input cannot retain the old pointer type.
    init(1);OP(83,10,90,20);OP(65,13,91,90,41);OP(61,1,92,91);
    assert(vk_spv_vertex_inputs(words,count,2,&out,&n)==1);free(out);
    init(1);OP(169,10,90,45,20,30);assert(vk_spv_vertex_inputs(words,count,2,&out,&n)==-1&&!out);
    assert(vk_spv_vertex_inputs(words,count,2|(2ull<<3),&out,&n)==1);free(out);
    init(1);OP(245,10,90,20,52,91,53);OP(83,10,91,90);assert(vk_spv_vertex_inputs(words,count,2,&out,&n)==1);free(out);
    init(1);OP(57,1,90,55,20);assert(vk_spv_vertex_inputs(words,count,2,&out,&n)==-1&&!out);
    init(1);OP(62,30,20);assert(vk_spv_vertex_inputs(words,count,2,&out,&n)==-1&&!out);
    init(1);OP(124,2,90,20);assert(vk_spv_vertex_inputs(words,count,2,&out,&n)==-1&&!out);
    init(1);words[count++]=0;assert(vk_spv_vertex_inputs(words,count,2,&out,&n)==-1&&!out);
    init(1);words[3]=UINT32_MAX;assert(vk_spv_vertex_inputs(words,count,2,&out,&n)==-1&&!out);
    for(unsigned failure=1;failure<=5;failure++){init(1);allocation=0;fail_allocation=failure;assert(vk_spv_vertex_inputs(words,count,2,&out,&n)==-2&&!out);}
    puts("PASS");return 0;
}
