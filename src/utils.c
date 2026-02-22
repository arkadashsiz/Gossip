#include "utils.h"
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ============================================================
 * Portable SHA-256 (no external deps)
 * ============================================================ */

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(e,f,g)   (((e)&(f))^(~(e)&(g)))
#define MAJ(a,b,c)  (((a)&(b))^((a)&(c))^((b)&(c)))
#define EP0(x)  (ROTR32(x,2) ^ROTR32(x,13)^ROTR32(x,22))
#define EP1(x)  (ROTR32(x,6) ^ROTR32(x,11)^ROTR32(x,25))
#define SIG0(x) (ROTR32(x,7) ^ROTR32(x,18)^((x)>>3))
#define SIG1(x) (ROTR32(x,17)^ROTR32(x,19)^((x)>>10))

typedef struct { uint8_t data[64]; uint32_t dlen; uint64_t blen; uint32_t s[8]; } S256;

static void s256_transform(S256 *c, const uint8_t d[]) {
    uint32_t a,b,cc,dd,e,f,g,h,t1,t2,m[64]; int i,j;
    for(i=0,j=0;i<16;i++,j+=4)
        m[i]=((uint32_t)d[j]<<24)|((uint32_t)d[j+1]<<16)|((uint32_t)d[j+2]<<8)|d[j+3];
    for(;i<64;i++) m[i]=SIG1(m[i-2])+m[i-7]+SIG0(m[i-15])+m[i-16];
    a=c->s[0];b=c->s[1];cc=c->s[2];dd=c->s[3];e=c->s[4];f=c->s[5];g=c->s[6];h=c->s[7];
    for(i=0;i<64;i++){t1=h+EP1(e)+CH(e,f,g)+K256[i]+m[i];t2=EP0(a)+MAJ(a,b,cc);
        h=g;g=f;f=e;e=dd+t1;dd=cc;cc=b;b=a;a=t1+t2;}
    c->s[0]+=a;c->s[1]+=b;c->s[2]+=cc;c->s[3]+=dd;
    c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
}

static void s256_init(S256 *c){
    c->dlen=0;c->blen=0;
    c->s[0]=0x6a09e667;c->s[1]=0xbb67ae85;c->s[2]=0x3c6ef372;c->s[3]=0xa54ff53a;
    c->s[4]=0x510e527f;c->s[5]=0x9b05688c;c->s[6]=0x1f83d9ab;c->s[7]=0x5be0cd19;
}

static void s256_update(S256 *c, const uint8_t *data, size_t len){
    for(size_t i=0;i<len;i++){
        c->data[c->dlen++]=data[i];
        if(c->dlen==64){s256_transform(c,c->data);c->blen+=512;c->dlen=0;}
    }
}

static void s256_final(S256 *c, uint8_t h[32]){
    uint32_t i=c->dlen;
    if(c->dlen<56){c->data[i++]=0x80;while(i<56)c->data[i++]=0;}
    else{c->data[i++]=0x80;while(i<64)c->data[i++]=0;s256_transform(c,c->data);memset(c->data,0,56);}
    c->blen+=c->dlen*8;
    c->data[63]=(uint8_t)c->blen;c->data[62]=(uint8_t)(c->blen>>8);
    c->data[61]=(uint8_t)(c->blen>>16);c->data[60]=(uint8_t)(c->blen>>24);
    c->data[59]=(uint8_t)(c->blen>>32);c->data[58]=(uint8_t)(c->blen>>40);
    c->data[57]=(uint8_t)(c->blen>>48);c->data[56]=(uint8_t)(c->blen>>56);
    s256_transform(c,c->data);
    for(i=0;i<4;i++){h[i]=(c->s[0]>>(24-i*8))&0xff;h[i+4]=(c->s[1]>>(24-i*8))&0xff;
        h[i+8]=(c->s[2]>>(24-i*8))&0xff;h[i+12]=(c->s[3]>>(24-i*8))&0xff;
        h[i+16]=(c->s[4]>>(24-i*8))&0xff;h[i+20]=(c->s[5]>>(24-i*8))&0xff;
        h[i+24]=(c->s[6]>>(24-i*8))&0xff;h[i+28]=(c->s[7]>>(24-i*8))&0xff;}
}

static void sha256_hex(const char *node_id, unsigned long nonce, char *hex_out){
    char input[256]; int ilen=snprintf(input,sizeof(input),"%s%lu",node_id,nonce);
    S256 ctx; s256_init(&ctx); s256_update(&ctx,(const uint8_t*)input,(size_t)ilen);
    uint8_t digest[32]; s256_final(&ctx,digest);
    for(int i=0;i<32;i++) snprintf(hex_out+i*2,3,"%02x",digest[i]);
    hex_out[64]='\0';
}

/* ============================================================ */

uint64_t current_time_ms(void){
    struct timeval tv; gettimeofday(&tv,NULL);
    return (uint64_t)tv.tv_sec*1000+(uint64_t)tv.tv_usec/1000;
}

int pow_check(const char *node_id, unsigned long nonce, int difficulty, char *digest_hex_out){
    char hex[65]; sha256_hex(node_id,nonce,hex);
    if(digest_hex_out) memcpy(digest_hex_out,hex,65);
    for(int i=0;i<difficulty;i++) if(hex[i]!='0') return 0;
    return 1;
}

unsigned long pow_mine(const char *node_id, int difficulty,
                       unsigned long *nonce_out, char *digest_hex_out){
    unsigned long nonce=0; char hex[65];
    while(1){
        sha256_hex(node_id,nonce,hex);
        int ok=1; for(int i=0;i<difficulty;i++) if(hex[i]!='0'){ok=0;break;}
        if(ok){*nonce_out=nonce;if(digest_hex_out)memcpy(digest_hex_out,hex,65);return nonce+1;}
        nonce++;
    }
}
