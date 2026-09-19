// Ulrich Drepper's published SHA-crypt vectors, against kernel/sha_crypt.c.
//
// Host unit test rather than a guest one because this is host code: it exists
// precisely so that NATIVE programs can check a guest password, and a native
// program never goes through the emulator.
//
// The vectors are the whole point. A SHA-crypt that gets the digest-B feed,
// the 16 + A[0] salt rehash or the output permutation wrong still returns
// confident-looking hashes of the right shape and length -- it just never
// matches what shadow-utils stored. Only known-answer tests catch that, which
// is why the refusal this replaced was the safe default until they existed.
#include <stdio.h>
#include <string.h>
#include "kernel/sha_crypt.h"

static int fails = 0;
static void vec(const char *salt, const char *key, const char *want) {
    char buf[256];
    char *got = aok_sha_crypt(key, salt, buf, sizeof(buf));
    int ok = got != NULL && strcmp(got, want) == 0;
    printf("%s  %.20s...\n", ok ? "PASS" : "FAIL", salt);
    if (!ok) {
        printf("      want %s\n      got  %s\n", want, got ? got : "(NULL)");
        fails++;
    }
}
int main(void) {
    vec("$6$saltstring", "Hello world!",
        "$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1");
    vec("$6$rounds=10000$saltstringsaltstring", "Hello world!",
        "$6$rounds=10000$saltstringsaltst$OW1/O6BYHV6BcXZu8QVeXbDWra3Oeqh0sbHbbMCVNSnCM/UrjmM0Dp8vOuZeHBy/YTBmSK6H9qs/y3RnOaw5v.");
    vec("$5$saltstring", "Hello world!",
        "$5$saltstring$5B8vYYiY.CVt1RlTTf8KbXBH3hsxY/GNooZaBBGWEc5");
    vec("$5$rounds=10000$saltstringsaltstring", "Hello world!",
        "$5$rounds=10000$saltstringsaltst$3xv.VbSHBb41AL9AvLeujZkZRBAwqFMz2.opqey6IcA");
    // Unsupported salts must stay a refusal, not a guess.
    char b[256];
    printf("%s  $1$ (unsupported) refused\n",
           aok_sha_crypt("x", "$1$abc", b, sizeof(b)) == NULL ? "PASS" : "FAIL");
    printf("%s  $y$ (yescrypt) refused\n",
           aok_sha_crypt("x", "$y$j9T$abc", b, sizeof(b)) == NULL ? "PASS" : "FAIL");
    printf("\n%s\n", fails ? "VECTORS FAILED" : "all vectors match");
    return fails != 0;
}
