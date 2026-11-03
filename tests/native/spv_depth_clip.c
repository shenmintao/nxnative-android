#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "spv_depth_clip.h"
int main(int argc,char **argv) {
 if(argc!=5)return 2;
 FILE*f=fopen(argv[1],"rb");if(!f)return 3;fseek(f,0,SEEK_END);long bytes=ftell(f);rewind(f);if(bytes<20||(bytes&3))return 4;
 uint32_t*w=malloc(bytes),*out=NULL,nw=bytes/4;assert(fread(w,1,bytes,f)==(size_t)bytes);fclose(f);vk_spv_depth_info info;
 int status=vk_spv_depth_clip(w,nw,atoi(argv[4]),atoi(argv[3]),&out,&nw,&info);
 if(status<0){fprintf(stderr,"depth patch failed: %d\n",status);return 5;}
 f=fopen(argv[2],"wb");if(!f)return 6;assert(fwrite(out?out:w,4,nw,f)==nw);fclose(f);
 printf("status=%d positions=%u captured=%u\n",status,info.positions,info.captured_positions);free(w);free(out);return 0;
}
