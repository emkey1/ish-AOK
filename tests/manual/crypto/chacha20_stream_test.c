// The ChaCha20 stream accelerator (system call 0xacc0) against OpenSSL's
// EVP_chacha20 on 200 random keys, nonces and lengths, and the two timed.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
extern long syscall(long,...);
#define NR 0xacc0
struct req{uint32_t op,alg;uint64_t key,nonce,aad,aadlen,in,inlen,out,tag;};
static long accel_stream(const uint8_t*k,const uint8_t*iv,const uint8_t*in,uint64_t len,uint8_t*out){
    struct req r={.op=0,.alg=1,.key=(uint64_t)k,.nonce=(uint64_t)iv,.in=(uint64_t)in,.inlen=len,.out=(uint64_t)out};
    return syscall(NR,&r);
}
typedef void EVP_CIPHER; typedef void EVP_CIPHER_CTX; typedef void ENGINE;
extern const EVP_CIPHER *EVP_chacha20(void);
extern EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void); extern void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX*);
extern int EVP_EncryptInit_ex(EVP_CIPHER_CTX*,const EVP_CIPHER*,ENGINE*,const uint8_t*,const uint8_t*);
extern int EVP_EncryptUpdate(EVP_CIPHER_CTX*,uint8_t*,int*,const uint8_t*,int);
static void ossl(const uint8_t*k,const uint8_t*iv,const uint8_t*in,int len,uint8_t*out){
    EVP_CIPHER_CTX*c=EVP_CIPHER_CTX_new();int l; EVP_EncryptInit_ex(c,EVP_chacha20(),0,k,iv);
    EVP_EncryptUpdate(c,out,&l,in,len); EVP_CIPHER_CTX_free(c);
}
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char**argv){
    uint8_t k[32],iv[16],pin[9000],p1[9000],p2[9000];
    if(accel_stream(k,iv,(const uint8_t*)"",0,p1)!=0){printf("stream accel unavailable\n");return 2;}
    srand(5); int fails=0,tot=0;
    for(int rep=0;rep<200;rep++){int len=rand()%9000;
        for(int i=0;i<32;i++)k[i]=rand();for(int i=0;i<16;i++)iv[i]=rand();for(int i=0;i<len;i++)pin[i]=rand();
        accel_stream(k,iv,pin,len,p1); ossl(k,iv,pin,len,p2); tot++; if(memcmp(p1,p2,len))fails++;}
    printf("raw chacha20 accel vs OpenSSL EVP_chacha20: %d cases, %d fails -> %s\n",tot,fails,fails?"FAIL":"PASS");
    // throughput
    int sz=16384; long it=argc>1?atol(argv[1]):20000; static uint8_t big[16384],ob[16384]; memset(big,0x5a,sz);
    double s=now();for(long i=0;i<it;i++)accel_stream(k,iv,big,sz,ob);double ea=now();
    double s2=now();for(long i=0;i<it;i++)ossl(k,iv,big,sz,ob);double eo=now();
    double mb=(double)sz*it/1e6;
    printf("throughput: accel %.0f MB/s  emulated %.0f MB/s  speedup %.1fx\n",mb/(ea-s),mb/(eo-s2),(eo-s2)/(ea-s));
    return fails?1:0;
}
