/* Offline C prototype. ASTC 4x4, one partition, direct RGB/L endpoints.
 * Endpoint and index arithmetic is verified independently with Arm astcenc.
 * Status: 0 direct; 1 BC1 three-colour; 2 BC4 six-value; 3 SNORM;
 *         4 direct-mode quantization cannot meet the one-byte error bound.
 * Fallback outputs are valid zero void-extent placeholders, not encodings.
 */
static unsigned rd16(const uint8_t *p) { return p[0] | (unsigned)p[1]<<8; }
static unsigned rd32(const uint8_t *p) { return rd16(p) | rd16(p+2)<<16; }
static void putbits(uint8_t out[16], unsigned value, unsigned n, unsigned bit) {
    for (unsigned i=0; i<n; i++,bit++) out[bit/8] |= ((value>>i)&1)<<(bit%8);
}
static void putweight(uint8_t out[16], unsigned v, unsigned n, unsigned index) {
    for (unsigned i=0;i<n;i++) { unsigned bit=127-index*n-i; out[bit/8] |= ((v>>i)&1)<<(bit%8); }
}
static int astc_byte(int a,int b,int w,int srgb) {
    a = srgb ? a*256+128 : a*257;
    b = srgb ? b*256+128 : b*257;
    return (a*(64-w)+b*w+32)>>14; /* UNORM8 decode mode; sRGB RGB is always 8-bit. */
}
static int endpoint_error(int a,int b,const uint8_t *target,const int *weights,unsigned mask,int srgb,int count) {
    int max=0;
    for (int i=0;i<count;i++) if (mask&(1u<<i)) {
        int d=astc_byte(a,b,weights[i],srgb)-target[i]; if(d<0)d=-d; if(d>max)max=d;
    }
    return max;
}
static int endpoints(int *a,int *b,const uint8_t *target,const int *weights,unsigned mask,int srgb,int count,int adjust) {
    int best=endpoint_error(*a,*b,target,weights,mask,srgb,count);
    if (!adjust || best<=1) return best;
    int aa=*a,bb=*b,ba=aa,bbest=bb;
    for(int da=-2;da<=2;da++)for(int db=-2;db<=2;db++) {
        int x=aa+da,y=bb+db;if(x<0||x>255||y<0||y>255)continue;
        int e=endpoint_error(x,y,target,weights,mask,srgb,count);
        if(e<best){best=e;ba=x;bbest=y;}
    }
    *a=ba;*b=bbest;return best;
}
EXPORT void transcode_bc(unsigned fmt,const uint8_t *src,uint8_t *out,uint8_t *status,unsigned n,int adjust) {
    unsigned kind=fmt>>8; int srgb=(fmt&255)==6;
    static const int w4[4]={0,64,21,43},w8[8]={0,64,9,18,27,37,46,55};
    for(unsigned j=0;j<n;j++,src+=8,out+=16) {
        memset(out,0,16); out[0]=0xfc;out[1]=0xfd;memset(out+2,255,6); /* valid fallback marker */
        if((fmt&255)==2){status[j]=3;continue;}
        if(kind==0x1a) {
            unsigned c0=rd16(src),c1=rd16(src+2),indices=rd32(src+4);
            if(c0<=c1){status[j]=1;continue;}
            uint8_t fake[8],pixels[64],palette[3][4]; memcpy(fake,src,8);memset(fake+4,0xe4,4);
            bcdec_bc1(fake,pixels,16);
            unsigned mask=0; for(unsigned i=0;i<16;i++)mask|=1u<<((indices>>(2*i))&3);
            int a[3],b[3],err=0;
            for(int c=0;c<3;c++) {
                for(int i=0;i<4;i++)palette[c][i]=pixels[i*4+c];
                a[c]=palette[c][0];b[c]=palette[c][1];
                int e=endpoints(a+c,b+c,palette[c],w4,mask,srgb,4,adjust);if(e>err)err=e;
            }
            /* Raw mapping is retained in adjust=0 for a separate measured baseline. */
            if(adjust&&err>1){status[j]=4;continue;}
            int flip=a[0]+a[1]+a[2]>b[0]+b[1]+b[2];
            memset(out,0,16);putbits(out,0x42,11,0);putbits(out,8,4,13);
            for(int c=0;c<3;c++) {putbits(out,flip?b[c]:a[c],8,17+16*c);putbits(out,flip?a[c]:b[c],8,25+16*c);}
            static const unsigned q[4]={0,3,1,2};
            for(unsigned i=0;i<16;i++){unsigned v=q[(indices>>(2*i))&3];putweight(out,flip?3-v:v,2,i);}
            status[j]=0;
        } else {
            if(src[0]<=src[1]){status[j]=2;continue;}
            uint64_t indices=0;for(unsigned i=0;i<6;i++)indices|=(uint64_t)src[2+i]<<(8*i);
            uint8_t fake[8],palette[16];memcpy(fake,src,8);
            uint64_t pattern=0;for(unsigned i=0;i<16;i++)pattern|=(uint64_t)(i%8)<<(3*i);
            for(unsigned i=0;i<6;i++)fake[2+i]=pattern>>(8*i);
            bcdec_bc4(fake,palette,4,0);
            unsigned mask=0;for(unsigned i=0;i<16;i++)mask|=1u<<((indices>>(3*i))&7);
            int a=src[0],b=src[1],err=endpoints(&a,&b,palette,w8,mask,0,8,adjust);
            if(adjust&&err>1){status[j]=4;continue;}
            memset(out,0,16);putbits(out,0x53,11,0);putbits(out,a,8,17);putbits(out,b,8,25);
            static const unsigned q[8]={0,7,1,2,3,4,5,6};
            for(unsigned i=0;i<16;i++)putweight(out,q[(indices>>(3*i))&7],3,i);
            status[j]=0;
        }
    }
}
