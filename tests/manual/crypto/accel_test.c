// Guest test for the AOK crypto accelerator (ISH_SYS_AEAD syscall):
//  1. RFC 8439 2.8.2 vector (bit-exact correctness)
//  2. differential vs the guest's own OpenSSL over random inputs
//  3. throughput: accel syscall vs emulated OpenSSL EVP
// Build: gcc -O2 -o acc acctest.c -l:libcrypto.so.3
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
extern long syscall(long, ...);
#define ISH_SYS_AEAD 0xacc0

struct ish_aead_req {
    uint32_t op; uint32_t alg;
    uint64_t key, nonce, aad, aadlen, in, inlen, out, tag;
};
static long accel_seal(const uint8_t *k,const uint8_t *n,const uint8_t *aad,uint64_t aadl,
        const uint8_t *pt,uint64_t ptl,uint8_t *ct,uint8_t *tag){
    struct ish_aead_req r = { .op=0,.alg=0,
        .key=(uint64_t)k,.nonce=(uint64_t)n,.aad=(uint64_t)aad,.aadlen=aadl,
        .in=(uint64_t)pt,.inlen=ptl,.out=(uint64_t)ct,.tag=(uint64_t)tag };
    return syscall(ISH_SYS_AEAD, &r);
}
static long accel_open(const uint8_t *k,const uint8_t *n,const uint8_t *aad,uint64_t aadl,
        const uint8_t *ct,uint64_t ctl,const uint8_t *tag,uint8_t *pt){
    struct ish_aead_req r = { .op=1,.alg=0,
        .key=(uint64_t)k,.nonce=(uint64_t)n,.aad=(uint64_t)aad,.aadlen=aadl,
        .in=(uint64_t)ct,.inlen=ctl,.out=(uint64_t)pt,.tag=(uint64_t)tag };
    return syscall(ISH_SYS_AEAD, &r);
}

// OpenSSL EVP (forward-declared; no -dev headers in guest)
typedef void EVP_CIPHER; typedef void EVP_CIPHER_CTX; typedef void ENGINE;
extern const EVP_CIPHER *EVP_chacha20_poly1305(void);
extern EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void); extern void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX*);
extern int EVP_EncryptInit_ex(EVP_CIPHER_CTX*,const EVP_CIPHER*,ENGINE*,const uint8_t*,const uint8_t*);
extern int EVP_EncryptUpdate(EVP_CIPHER_CTX*,uint8_t*,int*,const uint8_t*,int);
extern int EVP_EncryptFinal_ex(EVP_CIPHER_CTX*,uint8_t*,int*);
extern int EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX*,int,int,void*);
#define GET_TAG 0x10
static void ossl_seal(const uint8_t*k,const uint8_t*n,const uint8_t*aad,int aadl,
        const uint8_t*pt,int ptl,uint8_t*ct,uint8_t*tag){
    EVP_CIPHER_CTX*c=EVP_CIPHER_CTX_new();int l;
    EVP_EncryptInit_ex(c,EVP_chacha20_poly1305(),0,k,n);
    EVP_EncryptUpdate(c,0,&l,aad,aadl);
    EVP_EncryptUpdate(c,ct,&l,pt,ptl);
    EVP_EncryptFinal_ex(c,ct+l,&l);
    EVP_CIPHER_CTX_ctrl(c,GET_TAG,16,tag);
    EVP_CIPHER_CTX_free(c);
}
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}

int main(int argc,char**argv){
    // availability check
    uint8_t k[32]={0},n[12]={0},probe_ct[1],probe_tag[16];
    long av = accel_seal(k,n,0,0,(const uint8_t*)"",0,probe_ct,probe_tag);
    if (av != 0) { printf("accel unavailable (syscall ret %ld) -- is ISH_CRYPTO_ACCEL=1?\n", av); return 2; }

    // 1. RFC 8439 vector
    static const uint8_t rk[32]={0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f};
    static const uint8_t rn[12]={0x07,0,0,0,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47};
    static const uint8_t ra[12]={0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7};
    static const char rp[]="Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    static const uint8_t ct0[16]={0xd3,0x1a,0x8d,0x34,0x64,0x8e,0x60,0xdb,0x7b,0x86,0xaf,0xbc,0x53,0xef,0x7e,0xc2};
    static const uint8_t tg[16]={0x1a,0xe1,0x0b,0x59,0x4f,0x09,0xe2,0x6a,0x7e,0x90,0x2e,0xcb,0xd0,0x60,0x06,0x91};
    uint8_t rct[128],rtag[16],rback[128];
    accel_seal(rk,rn,ra,sizeof ra,(const uint8_t*)rp,sizeof rp-1,rct,rtag);
    int rfc_ok = !memcmp(rct,ct0,16) && !memcmp(rtag,tg,16);
    long op_rc = accel_open(rk,rn,ra,sizeof ra,rct,sizeof rp-1,rtag,rback);
    int open_ok = op_rc==0 && !memcmp(rback,rp,sizeof rp-1);
    uint8_t badtag[16]; memcpy(badtag,rtag,16); badtag[0]^=1;
    int rej_ok = accel_open(rk,rn,ra,sizeof ra,rct,sizeof rp-1,badtag,rback)!=0;
    printf("rfc8439 vector: %s   round-trip: %s   tamper-reject: %s\n",
           rfc_ok?"PASS":"FAIL", open_ok?"PASS":"FAIL", rej_ok?"PASS":"FAIL");

    // 2. differential vs OpenSSL, random lengths
    int lens[]={0,1,15,16,17,63,64,65,255,256,1024,16384,65536}; int fails=0,tot=0;
    static uint8_t pt[70000],a[64],c1[70000],c2[70000],t1[16],t2[16];
    srand(99);
    for(int rep=0;rep<20;rep++) for(int li=0;li<(int)(sizeof lens/sizeof*lens);li++){
        int pl=lens[li],al=rand()%64;
        for(int i=0;i<32;i++)k[i]=rand(); for(int i=0;i<12;i++)n[i]=rand();
        for(int i=0;i<al;i++)a[i]=rand(); for(int i=0;i<pl;i++)pt[i]=rand();
        accel_seal(k,n,a,al,pt,pl,c1,t1); ossl_seal(k,n,a,al,pt,pl,c2,t2);
        tot++; if(memcmp(c1,c2,pl)||memcmp(t1,t2,16))fails++;
    }
    printf("differential vs OpenSSL: %d cases, %d mismatches -> %s\n",tot,fails,fails?"FAIL":"PASS");

    // 3. throughput (16KB records, ssh-like), accel vs emulated OpenSSL
    int recsz = 16384; long iters = argc>1?atol(argv[1]):20000;
    static uint8_t big[16384], obuf[16384], tag[16];
    memset(big,0x5a,sizeof big);
    double s=now(); for(long i=0;i<iters;i++) accel_seal(k,n,a,12,big,recsz,obuf,tag); double ea=now();
    double s2=now(); for(long i=0;i<iters;i++) ossl_seal(k,n,a,12,big,recsz,obuf,tag); double eo=now();
    double mb=(double)recsz*iters/1e6;
    printf("throughput (16KB recs): accel %.0f MB/s   emulated-openssl %.0f MB/s   speedup %.1fx\n",
           mb/(ea-s), mb/(eo-s2), (eo-s2)/(ea-s));
    return (rfc_ok&&open_ok&&rej_ok&&!fails)?0:1;
}
