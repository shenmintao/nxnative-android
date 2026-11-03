#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <vulkan/vulkan.h>
#include "spv_buffer_writes.h"
#include "spv_nonwritable.h"
/* SHADER TYPES */
static struct { int vertex_stores, has_xfb, descriptor_buffer; VkDevice dev; VkPhysicalDeviceProperties props; } V;
static pthread_mutex_t g_sh_mu = PTHREAD_MUTEX_INITIALIZER;
static vkshader_t *g_shaders;
static unsigned reds, modules;
static uint32_t captured[4096], captured_n;
static void log_message(const char *fmt, ...) { if (strstr(fmt, "[RED]")) reds++; }
#define VKLOG log_message
#define VKRED_ONCE(n,...) log_message(__VA_ARGS__)
static void die(const char *s) { fputs(s, stderr); abort(); }
typedef struct { uint64_t key; uint32_t stage, spv_off, spv_len, refl_off, refl_len, pad; } bent;
static uint8_t bundle[32768]; static const uint8_t *g_bundle = bundle; static bent entry;
static const bent *find(uint64_t key) { return key == entry.key ? &entry : NULL; }
static void bundle_auto(void) {}
static int set_of(int st) { return st == 5 ? 0 : st; }
static void *nvn_hm_get(vkshader_t **map, const void *key) { return *map; }
static void nvn_hm_put(vkshader_t **map, const void *key, void *value) { *map = value; }
static void spv_io_locs(const uint32_t *w, uint32_t n, uint32_t *a, uint32_t *b) { *a = *b = 0; }
static int vk_spv_sampler_use(const uint32_t *w, uint32_t n, const uint16_t *b, size_t nb, uint8_t *out) { return 1; }
static void vk_spv_uniform_extents(const uint32_t *w, uint32_t n, uint32_t *out, size_t nb) {}
static int vk_spv_vertex_outputs(const uint32_t *w, uint32_t n, uint32_t mask, uint32_t **out, uint32_t *nn) { *out = NULL; *nn = n; return 0; }
static uint32_t *vk_spv_trim_unused_distances(const uint32_t *w, uint32_t n, uint32_t *nn) { return NULL; }
static uint32_t *copy_words(const uint32_t *w, uint32_t n, uint32_t *nn) { uint32_t *out = malloc(n * 4); memcpy(out, w, n * 4); *nn = n; return out; }
static uint32_t *spv_int_images(const uint32_t *w, uint32_t n, const uint16_t *b, const uint8_t *k, int nb, uint32_t *nn) { return copy_words(w,n,nn); }
static uint32_t *spv_add_outputs(const uint32_t *w, uint32_t n, uint32_t mask, uint32_t *nn) { return copy_words(w,n,nn); }
static uint32_t *spv_frag_depth(const uint32_t *w, uint32_t n, uint32_t *nn) { return copy_words(w,n,nn); }
static uint32_t *spv_dual(const uint32_t *w, uint32_t n, uint32_t *nn) { return copy_words(w,n,nn); }
static int vk_spv_vertex_inputs(const uint32_t *w, uint32_t n, uint64_t vin, uint32_t **out, uint32_t *nn) { *out = copy_words(w,n,nn); return 1; }
static VkDescriptorSetLayout shader_layout(const VkDescriptorSetLayoutBinding *b, uint32_t n) { return (VkDescriptorSetLayout)(uintptr_t)1; }
static VkDescriptorType desc_type(int d) { return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }
static VkShaderStageFlags stage_flag(int st) { return VK_SHADER_STAGE_VERTEX_BIT; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out) {
    captured_n = (uint32_t)ci->codeSize / 4; assert(captured_n <= 4096); memcpy(captured, ci->pCode, ci->codeSize);
    *out = (VkShaderModule)(uintptr_t)++modules; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetLayoutSizeEXT(VkDevice d, VkDescriptorSetLayout s, VkDeviceSize *out) { *out = 0; }
VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetLayoutBindingOffsetEXT(VkDevice d, VkDescriptorSetLayout s, uint32_t b, VkDeviceSize *out) { *out = 0; }
/* SHADER FUNCTIONS */

static uint32_t words[4096], count;
#define OP(code,...) do { uint32_t a[]={__VA_ARGS__}; words[count++]=((uint32_t)(sizeof a/sizeof*a)+1)<<16|(code); memcpy(words+count,a,sizeof a); count+=(uint32_t)(sizeof a/sizeof*a); } while(0)
static unsigned decorated(const uint32_t *w, uint32_t n, uint32_t id) {
    unsigned result = 0;
    for(uint32_t at=5;at<n;at+=w[at]>>16) if((w[at]&65535)==71 && (w[at]>>16)==3 && w[at+1]==id && w[at+2]==24) result++;
    return result;
}
static void init(int modern, int writer) {
    memset(words,0,sizeof words); words[0]=0x07230203; words[1]=0x10300; words[3]=128; count=5;
    OP(17,1);OP(14,0,1);OP(15,0,50,0x6e69616d,0);
    OP(71,10,33,2);OP(71,20,33,3);OP(71,30,33,4);OP(71,40,33,99);
    OP(71,2,modern?2:3);OP(72,2,0,35,0);OP(71,5,2);
    OP(22,1,32);OP(30,2,1);OP(32,3,modern?12:2,2);OP(32,4,modern?12:2,1);
    OP(30,5,1);OP(32,6,2,5);OP(59,3,10,modern?12:2);OP(59,3,20,modern?12:2);OP(59,6,30,2);OP(59,3,40,modern?12:2);
    OP(54,1,50,0,60);OP(248,51);OP(65,4,70,10,90);OP(61,1,71,70);
    if(writer){OP(65,4,80,20,90);OP(62,80,71);}
    OP(253,0);OP(56,0); // semantic fixture; complete GLSL modules are validated separately
}
static void free_shader(vkshader_t *s) { free(s->var); free(s->spv); free(s); g_shaders=NULL; }
static vkshader_t *base(int stage, int feature, int writer) {
    init(1,writer); memset(bundle,0,sizeof bundle); entry=(bent){0x123,stage,1024,count*4,0,80,0};
    uint32_t hdr[5]={3,0,1,1,1}; memcpy(bundle,hdr,sizeof hdr);
    dmsv_bind b[3]={{.binding=2,.desc=RD_SSBO,.count=1},{.binding=3,.desc=RD_SSBO,.count=1},{.binding=4,.desc=RD_UBO,.count=1}};
    memcpy(bundle+20,b,sizeof b);memcpy(bundle+1024,words,count*4);V.vertex_stores=feature;V.has_xfb=1;
    reds=0;return vk_shader(entry.key,stage);
}
static int file_rewrite(char **argv) {
    FILE*f=fopen(argv[1],"rb");if(!f)return 2;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);if(n<20||(n&3))return 2;
    uint32_t*w=malloc((size_t)n),*out=NULL,nw=(uint32_t)n/4;if(fread(w,1,n,f)!=(size_t)n)return 2;fclose(f);
    uint16_t bindings[2]={2,3};uint8_t storage[2]={1,1},written[2];
    assert(vk_spv_buffer_writes(w,nw,bindings,storage,2,written)==1);assert(!written[0]&&written[1]);
    assert(vk_spv_nonwritable(w,nw,atoi(argv[3]),0,bindings,storage,written,2,&out,&nw)>=0);
    f=fopen(argv[2],"wb");if(!f)return 2;fwrite(out?out:w,4,nw,f);fclose(f);free(out);free(w);return 0;
}
int main(int argc,char **argv) {
    if(argc==4)return file_rewrite(argv);assert(argc==2);
    uint16_t bindings[3]={2,3,4};uint8_t storage[3]={1,1,0},written[3];uint32_t*out=NULL,n;
    for(int modern=0;modern<2;modern++) {
        init(modern,1);assert(vk_spv_buffer_writes(words,count,bindings,storage,3,written)==1);
        assert(!written[0]&&written[1]);
        assert(vk_spv_nonwritable(words,count,0,0,bindings,storage,written,3,&out,&n)==1);
        assert(decorated(out,n,10)==1&&!decorated(out,n,20)&&!decorated(out,n,30)&&!decorated(out,n,40));
        // Only annotation insertion: the executable instructions retain their bytes.
        uint32_t at=5,first=0;while(at<count){if((words[at]&65535)==22){first=at;break;}at+=words[at]>>16;}
        assert(!memcmp(out,words,first*4));assert(!memcmp(out+first+3,words+first,(count-first)*4));
        uint32_t*again=NULL,nn;assert(vk_spv_nonwritable(out,n,0,0,bindings,storage,written,3,&again,&nn)==0&&!again&&nn==n);free(out);
    }
    for(int stage=0;stage<6;stage++)for(int feature=0;feature<2;feature++) {
        vkshader_t*s=base(stage,feature,1);assert(s);
        int patch=!feature&&stage!=1&&stage!=5;
        assert(decorated(captured,captured_n,10)==(unsigned)patch&&!decorated(captured,captured_n,20));assert(reds==(unsigned)patch);
        if(!patch)assert(captured_n==count&&!memcmp(captured,words,count*4));
        // Actual shader_module path creates and caches variants from the retained base.
        unsigned created=modules;VkShaderModule m=shader_module(s,1,0,0,0,0);assert(m&&m!=s->mod&&modules==created+1);
        assert(decorated(captured,captured_n,10)==(unsigned)patch&&!decorated(captured,captured_n,20));
        assert(shader_module(s,1,0,0,0,0)==m&&modules==created+1&&reds==(unsigned)patch);
        shader_module(s,0,0,1,0,0);assert(decorated(captured,captured_n,10)==(unsigned)patch);
        shader_module(s,0,0,0,0,1);assert(decorated(captured,captured_n,10)==(unsigned)patch);
        free_shader(s);
    }
    // The alternate file may write a buffer that the base only reads. It is
    // reanalysed rather than trusting the base's read-only mask.
    vkshader_t*s=base(0,0,0);assert(!reds);init(1,1);
    char path[1024];snprintf(path,sizeof path,"%s/%016llx.spv",argv[1],(unsigned long long)s->key);
    FILE*f=fopen(path,"wb");assert(f);fwrite(words,4,count,f);fclose(f);
#ifdef _WIN32
    _putenv_s("DM_VK_SPV_DIR",argv[1]);
#else
    setenv("DM_VK_SPV_DIR",argv[1],1);
#endif
    shader_module(s,0,0,1u<<31,0,0);assert(decorated(captured,captured_n,10)==1&&!decorated(captured,captured_n,20)&&reds==1);
    alt_module(s,0);assert(reds==1);remove(path);free_shader(s);
    // Unknown roots force the existing conservative mask; nothing is asserted read-only.
    init(1,0);OP(55,4,81);OP(62,81,71);assert(vk_spv_buffer_writes(words,count,bindings,storage,3,written)==2);
    assert(vk_spv_nonwritable(words,count,0,0,bindings,storage,written,3,&out,&n)==0&&!out);
    init(1,0);OP(71,90,33,2);OP(74,90,10);OP(71,10,24);written[0]=written[1]=written[2]=0;
    assert(vk_spv_nonwritable(words,count,0,0,bindings,storage,written,3,&out,&n)==1);assert(!decorated(out,n,30));free(out);
    init(1,0);words[count++]=0;assert(vk_spv_nonwritable(words,count,0,0,bindings,storage,written,3,&out,&n)==-1&&!out);
    puts("PASS");return 0;
}
