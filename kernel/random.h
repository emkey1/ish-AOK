#ifndef KERNEL_RANDOM_H
#define KERNEL_RANDOM_H

#include <stdlib.h>

int get_random(char *buf, size_t len);

// Emulated entropy pool size in bits, reported by the /dev/{u,}random ioctls
// and /proc/sys/kernel/random/{poolsize,entropy_avail}. iSH draws randomness
// from the host CSPRNG, so the pool is always treated as full. 4096 matches the
// input-pool size of the 5.10 kernel AOK advertises (kernel/uname.c); 5.18
// shrank it to 256.
#define RANDOM_POOL_BITS 4096

// ioctls on /dev/random and /dev/urandom (linux/random.h). Standard asm-generic
// _IOC encoding; identical for the i386 and amd64 ABIs (the rand_pool_info
// header is two 32-bit ints on both).
#define RNDGETENTCNT_   0x80045200 // _IOR('R', 0x00, int)        get entropy count
#define RNDADDTOENTCNT_ 0x40045201 // _IOW('R', 0x01, int)        credit entropy
#define RNDADDENTROPY_  0x40085203 // _IOW('R', 0x03, int[2]+buf) add + credit entropy
#define RNDZAPENTCNT_   0x00005204 // _IO('R', 0x04)              zero entropy count
#define RNDCLEARPOOL_   0x00005206 // _IO('R', 0x06)              clear pool
#define RNDRESEEDCRNG_  0x00005207 // _IO('R', 0x07)              reseed CRNG

#endif
