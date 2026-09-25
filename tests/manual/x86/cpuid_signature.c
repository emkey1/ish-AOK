// cpuid_signature -- CPUID leaf 1's signature and CLFLUSH line size are ones
// real software accepts, and /proc/cpuinfo says the same thing CPUID does.
//
// Leaf 1 EAX used to be 0: family 0, model 0. That is not a harmless
// "unknown". HotSpot's VM_Version::get_processor_features reads the feature
// bits only when the family is above 4 (a 486 or older has no CPUID worth
// believing), so every JVM saw no SSE2 and the x86_64 build refused to start:
// "Unknown x64 processor: SSE2 not supported". Past that, its x86_64 build
// guarantees the CLFLUSH bit and a CLFLUSH line size of 8 quadwords (leaf 1
// EBX bits 15:8), and neither was set. Every x86_64 part has both.
//
// Meanwhile /proc/cpuinfo printed its own family and model -- 6 and 85 for a
// 64-bit guest, 1 and 1 for i386 -- so the two views of one CPU disagreed.
// Linux derives the /proc fields from CPUID, so they must match here, decoded
// the way Linux decodes them (x86_family()/x86_model()).
//
// x86 only; builds and runs on both guests.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../test_common.h"

#if !defined(__i386__) && !defined(__x86_64__)
int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    printf("cpuid_signature: SKIP (x86 only)\n");
    return 0;
}
#else

#include <cpuid.h>

#define CLFLUSH_BIT (1u << 19)

// The first processor's value for `key`, or NULL. Keys are matched up to the
// tab-and-colon padding Linux uses ("cpu family\t: 6").
static char *cpuinfo_field(const char *key, char *out, size_t out_size) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f == NULL)
        return NULL;
    char line[4096];
    char *found = NULL;
    size_t key_len = strlen(key);
    while (fgets(line, sizeof line, f) != NULL) {
        if (line[0] == '\n')
            break; // end of the first processor's block
        if (strncmp(line, key, key_len) != 0)
            continue;
        char *p = line + key_len;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != ':')
            continue; // "model" must not match "model name"
        p++;
        while (*p == ' ')
            p++;
        p[strcspn(p, "\n")] = '\0';
        snprintf(out, out_size, "%s", p);
        found = out;
        break;
    }
    fclose(f);
    return found;
}

static void expect_field_num(const char *key, unsigned long want) {
    char buf[4096];
    if (cpuinfo_field(key, buf, sizeof buf) == NULL) {
        failf(key, 0, 0, 0, want, 0, 0);
        printf("  /proc/cpuinfo has no \"%s\" line\n", key);
        return;
    }
    unsigned long got = strtoul(buf, NULL, 10);
    test_logf("  /proc/cpuinfo %-12s %lu (CPUID says %lu)\n", key, got, want);
    if (got != want)
        failf(key, got, 0, 0, want, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    unsigned a, b, c, d;
    __cpuid(0, a, b, c, d);
    unsigned max_leaf = a;
    char vendor[13];
    memcpy(vendor + 0, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = '\0';

    __cpuid(1, a, b, c, d);
    unsigned family = (a >> 8) & 0xf;
    if (family == 0xf)
        family += (a >> 20) & 0xff;
    unsigned model = (a >> 4) & 0xf;
    if (family >= 6)
        model |= ((a >> 16) & 0xf) << 4;
    unsigned stepping = a & 0xf;
    unsigned clflush_qwords = (b >> 8) & 0xff;
    test_logf("vendor %s, max leaf %#x, signature %#x: family %u model %#x "
              "stepping %u; clflush %s, line %u bytes\n",
              vendor, max_leaf, a, family, model, stepping,
              (d & CLFLUSH_BIT) ? "yes" : "no", clflush_qwords * 8);

    // HotSpot: `if (cpu_family() > 4) { _features = feature_flags(); ... }`.
    if (family <= 4)
        failf("family above 4", family, 0, 0, 5, 0, 0);

    // A line size only means something with the bit, and the bit is useless
    // without one.
    if ((d & CLFLUSH_BIT) && clflush_qwords == 0)
        failf("clflush line size", 0, 0, 0, 8, 0, 0);
#if defined(__x86_64__)
    // HotSpot's x86_64 guarantees, verbatim in effect.
    if (!(d & CLFLUSH_BIT))
        failf("clflush advertised", 0, 0, 0, 1, 0, 0);
    if (clflush_qwords != 8)
        failf("clflush line is 8 quadwords", clflush_qwords, 0, 0, 8, 0, 0);
#endif

    char buf[4096];
    if (cpuinfo_field("vendor_id", buf, sizeof buf) == NULL || strcmp(buf, vendor) != 0) {
        printf("  /proc/cpuinfo vendor_id \"%s\", CPUID \"%s\"\n",
               cpuinfo_field("vendor_id", buf, sizeof buf) ? buf : "(none)", vendor);
        failf("vendor_id", 0, 0, 0, 1, 0, 0);
    }
    expect_field_num("cpu family", family);
    expect_field_num("model", model);
    expect_field_num("stepping", stepping);
    expect_field_num("cpuid level", max_leaf);
    if (d & CLFLUSH_BIT)
        expect_field_num("clflush size", clflush_qwords * 8);

    // The flags line names clflush exactly when CPUID sets the bit.
    if (cpuinfo_field("flags", buf, sizeof buf) == NULL) {
        failf("flags line", 0, 0, 0, 1, 0, 0);
    } else {
        char padded[4200];
        snprintf(padded, sizeof padded, " %s ", buf);
        int listed = strstr(padded, " clflush ") != NULL;
        int set = (d & CLFLUSH_BIT) != 0;
        test_logf("  /proc/cpuinfo flags %s clflush; CPUID %s it\n",
                  listed ? "lists" : "omits", set ? "sets" : "clears");
        if (listed != set)
            failf("flags clflush", listed, 0, 0, set, 0, 0);
    }
    return finish_suite("cpuid_signature");
}
#endif
