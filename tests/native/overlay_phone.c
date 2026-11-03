#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "overlay_layout.h"
static float test_W,test_H,test_dpi,test_insets[4],test_percent=100;
int dm_overlay_layout(void){return test_W/test_H>=OV_PHONE_ASPECT;}
int dm_overlay_custom(int l,int i,float*x,float*y,float*s){(void)l;(void)i;*x=*y=0;*s=1;return 0;}
int dm_overlay_button_size(void){return (int)test_percent;}
float dm_overlay_scale(void){
 float face=0.1f;const ov_ctl*L=OV_LAYOUTS[dm_overlay_layout()];
 for(int i=0;i<OV_N;i++)if(L[i].kind==OV_BUTTON&&L[i].btn==DM_BTN_A)face=L[i].w;
 float mm=face*test_H/test_dpi*25.4f,s=OV_MIN_MM/mm;
 return fmaxf(1,fminf(1.45f,s));
}
void dm_overlay_metrics(float W,float H,float*mm,float insets[4],float*percent){
 *mm=test_dpi/25.4f*(H/test_H);*percent=test_percent;
 for(int k=0;k<4;k++)insets[k]=test_insets[k]*(k&1?H/test_H:W/test_W);
}
/* SPRITE_SCALE */
int main(int argc,char**argv){
 assert(argc==10);test_W=atof(argv[1]);test_H=atof(argv[2]);test_dpi=atof(argv[3]);
 for(int i=0;i<4;i++)test_insets[i]=atof(argv[4+i]);test_percent=atof(argv[8]);int legacy=atoi(argv[9]),l=dm_overlay_layout();
 for(int i=0;i<OV_N;i++){
  const ov_ctl*c=&OV_LAYOUTS[l][i];float b[4],before[4]={c->ax*test_W+c->x*test_H,c->y*test_H,c->w*test_H*dm_overlay_scale()/2,c->h*test_H*dm_overlay_scale()/2};
  ov_box(c,test_W,test_H,b);
  ov_theme theme={.layout=l,.dh=l?412:760};float sprite=ov_control_sprite_scale(&theme,c,b,test_H);
  if(l)assert(fabsf(sprite*c->h*theme.dh-2*b[3])<.001f);
  else if(test_percent==100)assert(sprite==test_H/theme.dh*dm_overlay_scale()*ov_cscale(c));
  if(!l&&test_percent==100&&test_insets[0]==0&&test_insets[1]==0&&test_insets[2]==0&&test_insets[3]<=100)
   assert(!memcmp(b,before,sizeof b)); // exact default tablet geometry, not just near-equality
  if(legacy)memcpy(b,before,sizeof b);
  printf("%s %d %.9g %.9g %.9g %.9g\n",c->name,c->kind,b[0],b[1],b[2],b[3]);
 }
 return 0;
}
