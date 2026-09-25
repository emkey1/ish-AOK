// The vDSO's clock functions, called directly and through the C library,
// against the system calls they stand in for.
//
// A 64-bit guest used to have no vDSO at all, so every clock read was a full
// system call -- 700 ns on arm64, the cost of getppid. vdso/<arch>/vdso.S is
// the fix; this test is what it has to keep true, and every check in it holds
// on Linux (camd, x86_64 and -m32) as written:
//
//  - The vDSO is there (AT_SYSINFO_EHDR) and the symbols the C libraries look
//    up resolve by NAME and VERSION through DT_HASH, the way musl's
//    __vdsosym finds them. musl reads DT_HASH and nothing else.
//  - Every clock it serves is bracketed by the system call: raw read, vDSO
//    read, raw read, and the middle one lies between the other two, for each
//    clock, thousands of times. Not a tolerance -- a vDSO that ran behind the
//    kernel by any amount would fall out of the bracket.
//  - The clocks it does not serve (the CPU-time ones, the dynamic id
//    clock_getcpuclockid builds, an id Linux does not have) come back exactly
//    as the system call answers them, errors included.
//  - gettimeofday, with and without a timezone; clock_getres.
//  - A file's mtime and ctime are never later than a vDSO read taken after
//    the write that set them -- on the root filesystem and on a tmpfs.
//  - MONOTONIC never goes backward, within a thread or across threads.
//  - It is not a system call: under a seccomp filter that fails
//    clock_gettime and gettimeofday with EPERM, both still work through the
//    vDSO and through the C library (where it uses the vDSO: not musl on
//    riscv64) while the raw calls fail; and a tracer sees no syscall stops for
//    a run of vDSO reads, but sees a run of raw ones (the control).
//  - Each process has its own copy: a patch to one process's vDSO -- through
//    /proc/self/mem as a debugger plants a breakpoint, or by mprotecting it
//    writable and storing -- reaches neither its forked parent nor a freshly
//    exec'd process; a MAP_SHARED page and a shared file mapping, patched the
//    same way, are the controls that must be seen. And the patching process
//    still sees its whole vDSO as one [vdso] line in /proc/self/maps, as on
//    Linux. (AOK's i386 vDSO was ONE host array mapped into every process, so
//    the mprotect route let any process rewrite the clock_gettime every other
//    process ran, root's included.)
//
// On AOK's i386 guest the vDSO is still a thin wrapper around int $0x80
// (vdso/vdso.c), so the two "not a system call" checks are skipped there,
// with a line saying so. Everything else runs on every architecture.
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include "test_common.h"

#define NAME "vdso_clock"

#ifndef CLOCK_TAI
#define CLOCK_TAI 11
#endif

// The kernel's own layouts, so a raw call never depends on the C library's
// idea of time_t (32-bit glibc, -m32, still defaults to a 32-bit one).
struct kts64 { int64_t sec; int64_t nsec; };
struct kts32 { int32_t sec; int32_t nsec; };
struct ktv { long sec; long usec; };  // struct timeval, native word size

#if UINTPTR_MAX == 0xffffffffu
typedef Elf32_Ehdr Ehdr; typedef Elf32_Phdr Phdr; typedef Elf32_Sym Sym;
typedef Elf32_Verdef Verdef; typedef Elf32_Verdaux Verdaux; typedef Elf32_Dyn Dyn;
typedef Elf32_Word HashWord;
#define ELF_ST_TYPE_ ELF32_ST_TYPE
#define ELF_ST_BIND_ ELF32_ST_BIND
#else
typedef Elf64_Ehdr Ehdr; typedef Elf64_Phdr Phdr; typedef Elf64_Sym Sym;
typedef Elf64_Verdef Verdef; typedef Elf64_Verdaux Verdaux; typedef Elf64_Dyn Dyn;
typedef Elf64_Word HashWord;
#define ELF_ST_TYPE_ ELF64_ST_TYPE
#define ELF_ST_BIND_ ELF64_ST_BIND
#endif

// The names and version each architecture's C library asks for.
#if defined(__aarch64__)
#define VDSO_VERSION "LINUX_2.6.39"
#define VDSO_GETTIME "__kernel_clock_gettime"
#define VDSO_GETTOD "__kernel_gettimeofday"
#define VDSO_GETRES "__kernel_clock_getres"
#elif defined(__riscv)
#define VDSO_VERSION "LINUX_4.15"
#define VDSO_GETTIME "__vdso_clock_gettime"
#define VDSO_GETTOD "__vdso_gettimeofday"
#define VDSO_GETRES "__vdso_clock_getres"
#else // x86_64 and i386
#define VDSO_VERSION "LINUX_2.6"
#define VDSO_GETTIME "__vdso_clock_gettime"
#define VDSO_GETTOD "__vdso_gettimeofday"
#define VDSO_GETRES "__vdso_clock_getres"
#endif

// The raw system calls, by whatever name this libc gives them.
#if defined(SYS_clock_gettime64)
#define RAW_GETTIME64 SYS_clock_gettime64
#elif defined(SYS_clock_gettime) && UINTPTR_MAX != 0xffffffffu
#define RAW_GETTIME64 SYS_clock_gettime
#endif
#if defined(SYS_clock_getres_time64)
#define RAW_GETRES64 SYS_clock_getres_time64
#elif defined(SYS_clock_getres) && UINTPTR_MAX != 0xffffffffu
#define RAW_GETRES64 SYS_clock_getres
#endif
#if defined(SYS_gettimeofday)
#define RAW_GETTOD SYS_gettimeofday
#elif defined(SYS_gettimeofday_time32)
#define RAW_GETTOD SYS_gettimeofday_time32
#endif
// Every number a filter has to refuse for "clock_gettime" to be refused.
static const long gettime_nrs[] = {
#if defined(SYS_clock_gettime)
    SYS_clock_gettime,
#endif
#if defined(SYS_clock_gettime32)
    SYS_clock_gettime32,
#endif
#if defined(SYS_clock_gettime64)
    SYS_clock_gettime64,
#endif
#if defined(RAW_GETTOD)
    RAW_GETTOD,
#endif
};

static int64_t kts64_ns(struct kts64 t) { return t.sec * 1000000000 + t.nsec; }

// -errno, or 0 with *ns the reading.
static int raw_gettime(clockid_t clock, int64_t *ns) {
    struct kts64 t;
    long r = syscall(RAW_GETTIME64, clock, &t);
    if (r < 0)
        return -errno;
    *ns = kts64_ns(t);
    return 0;
}

static int raw_getres(clockid_t clock, int64_t *ns) {
    struct kts64 t;
    long r = syscall(RAW_GETRES64, clock, &t);
    if (r < 0)
        return -errno;
    *ns = kts64_ns(t);
    return 0;
}

// ---- finding the vDSO's symbols, as musl does ----------------------------

static int checkver(Verdef *def, int vsym, const char *vername, char *strings) {
    vsym &= 0x7fff;
    for (;;) {
        if (!(def->vd_flags & VER_FLG_BASE) && (def->vd_ndx & 0x7fff) == vsym)
            break;
        if (def->vd_next == 0)
            return 0;
        def = (Verdef *) ((char *) def + def->vd_next);
    }
    Verdaux *aux = (Verdaux *) ((char *) def + def->vd_aux);
    return strcmp(vername, strings + aux->vda_name) == 0;
}

static int vdso_has_hash;

static void *vdso_sym(const char *vername, const char *name) {
    uintptr_t base_addr = (uintptr_t) getauxval(AT_SYSINFO_EHDR);
    if (base_addr == 0)
        return NULL;
    Ehdr *eh = (Ehdr *) base_addr;
    Phdr *ph = (Phdr *) ((char *) eh + eh->e_phoff);
    Dyn *dynv = NULL;
    uintptr_t base = (uintptr_t) -1;
    for (int i = 0; i < eh->e_phnum; i++, ph = (Phdr *) ((char *) ph + eh->e_phentsize)) {
        if (ph->p_type == PT_LOAD && base == (uintptr_t) -1)
            base = (uintptr_t) eh + ph->p_offset - ph->p_vaddr;
        else if (ph->p_type == PT_DYNAMIC)
            dynv = (Dyn *) ((char *) eh + ph->p_offset);
    }
    if (dynv == NULL || base == (uintptr_t) -1)
        return NULL;
    char *strings = NULL;
    Sym *syms = NULL;
    HashWord *hashtab = NULL;
    uint16_t *versym = NULL;
    Verdef *verdef = NULL;
    for (Dyn *d = dynv; d->d_tag != DT_NULL; d++) {
        void *p = (void *) (base + d->d_un.d_ptr);
        switch (d->d_tag) {
            case DT_STRTAB: strings = p; break;
            case DT_SYMTAB: syms = p; break;
            case DT_HASH: hashtab = p; break;
            case DT_VERSYM: versym = p; break;
            case DT_VERDEF: verdef = p; break;
        }
    }
    vdso_has_hash = hashtab != NULL;
    if (strings == NULL || syms == NULL || hashtab == NULL)
        return NULL;
    if (verdef == NULL)
        versym = NULL;
    for (HashWord i = 0; i < hashtab[1]; i++) {
        int type = ELF_ST_TYPE_(syms[i].st_info), bind = ELF_ST_BIND_(syms[i].st_info);
        if (type != STT_FUNC && type != STT_NOTYPE)
            continue;
        if (bind != STB_GLOBAL && bind != STB_WEAK)
            continue;
        if (syms[i].st_shndx == 0 || strcmp(name, strings + syms[i].st_name) != 0)
            continue;
        if (versym != NULL && !checkver(verdef, versym[i], vername, strings))
            continue;
        return (void *) (base + syms[i].st_value);
    }
    return NULL;
}

// ---- the vDSO's functions, whichever layout this architecture's take -----

static void *vdso_gettime_fn, *vdso_gettod_fn, *vdso_getres_fn;

// The i386 vDSO's clock_gettime takes the 32-bit timespec; every 64-bit one
// takes the 64-bit one, which is also the C library's there.
static int vdso_gettime(clockid_t clock, int64_t *ns) {
#if UINTPTR_MAX == 0xffffffffu
    struct kts32 t = {-1, -1};
    int r = ((int (*)(clockid_t, struct kts32 *)) vdso_gettime_fn)(clock, &t);
    if (r == 0)
        *ns = (int64_t) t.sec * 1000000000 + t.nsec;
#else
    struct kts64 t = {-1, -1};
    int r = ((int (*)(clockid_t, struct kts64 *)) vdso_gettime_fn)(clock, &t);
    if (r == 0) {
        if (t.nsec < 0 || t.nsec >= 1000000000)
            return -1000;   // not a timespec at all
        *ns = kts64_ns(t);
    }
#endif
    return r;
}

static int vdso_getres(clockid_t clock, int64_t *ns) {
#if UINTPTR_MAX == 0xffffffffu
    struct kts32 t = {-1, -1};
    int r = ((int (*)(clockid_t, struct kts32 *)) vdso_getres_fn)(clock, &t);
    if (r == 0)
        *ns = (int64_t) t.sec * 1000000000 + t.nsec;
#else
    struct kts64 t = {-1, -1};
    int r = ((int (*)(clockid_t, struct kts64 *)) vdso_getres_fn)(clock, &t);
    if (r == 0)
        *ns = kts64_ns(t);
#endif
    return r;
}

static int vdso_gettod(struct ktv *tv, void *tz) {
    return ((int (*)(struct ktv *, void *)) vdso_gettod_fn)(tv, tz);
}

// Whether this C library reads the clock through the vDSO at all. glibc does
// everywhere. musl does everywhere but riscv64, whose port has the symbol
// commented out (arch/riscv64/syscall_arch.h), so there it makes the system
// call on Linux too.
static int libc_uses_vdso(void) {
#if !defined(__GLIBC__) && defined(__riscv)
    return 0;
#else
    return 1;
#endif
}

// AOK's i386 vDSO makes the system call (see the header comment); Linux's
// never does. uname says which kernel this is.
static int vdso_makes_syscalls(void) {
#if defined(__i386__)
    struct utsname u;
    return uname(&u) == 0 && strstr(u.release, "ish_aok") != NULL;
#else
    return 0;
#endif
}

// ---- 1. the vDSO and its symbols ------------------------------------------

static int check_symbols(void) {
    if (getauxval(AT_SYSINFO_EHDR) == 0) {
        printf("FAIL no AT_SYSINFO_EHDR: this process has no vDSO\n");
        failures_total++;
        return 0;
    }
    vdso_gettime_fn = vdso_sym(VDSO_VERSION, VDSO_GETTIME);
    vdso_gettod_fn = vdso_sym(VDSO_VERSION, VDSO_GETTOD);
    vdso_getres_fn = vdso_sym(VDSO_VERSION, VDSO_GETRES);
    if (!vdso_has_hash) {
        printf("FAIL the vDSO has no DT_HASH, which is all musl reads\n");
        failures_total++;
    }
    if (vdso_gettime_fn == NULL) {
        printf("FAIL %s@%s not found in the vDSO\n", VDSO_GETTIME, VDSO_VERSION);
        failures_total++;
    }
#if !defined(__i386__)
    // AOK's i386 vDSO has only clock_gettime, gettimeofday and time.
    if (vdso_gettod_fn == NULL) {
        printf("FAIL %s@%s not found in the vDSO\n", VDSO_GETTOD, VDSO_VERSION);
        failures_total++;
    }
    if (vdso_getres_fn == NULL) {
        printf("FAIL %s@%s not found in the vDSO\n", VDSO_GETRES, VDSO_VERSION);
        failures_total++;
    }
#endif
    // A wrong version must NOT resolve: the lookup has to be checking it.
    if (vdso_sym("LINUX_0.0", VDSO_GETTIME) != NULL) {
        printf("FAIL %s resolved under a version the vDSO does not define\n", VDSO_GETTIME);
        failures_total++;
    }
    test_logf("vdso at %#lx: %s=%p %s=%p %s=%p\n", getauxval(AT_SYSINFO_EHDR),
              VDSO_GETTIME, vdso_gettime_fn, VDSO_GETTOD, vdso_gettod_fn,
              VDSO_GETRES, vdso_getres_fn);
    return vdso_gettime_fn != NULL;
}

// ---- 2. every served clock, bracketed by the system call ------------------

static const struct { clockid_t id; const char *name; } served[] = {
    {CLOCK_REALTIME, "REALTIME"},
    {CLOCK_MONOTONIC, "MONOTONIC"},
    {CLOCK_MONOTONIC_RAW, "MONOTONIC_RAW"},
    {CLOCK_REALTIME_COARSE, "REALTIME_COARSE"},
    {CLOCK_MONOTONIC_COARSE, "MONOTONIC_COARSE"},
    {CLOCK_BOOTTIME, "BOOTTIME"},
    {CLOCK_TAI, "TAI"},
};

static void check_brackets(void) {
    for (size_t c = 0; c < sizeof(served) / sizeof(served[0]); c++) {
        clockid_t id = served[c].id;
        int64_t worst_early = 0, worst_late = 0;
        unsigned bad = 0;
        for (int i = 0; i < 3000; i++) {
            int64_t a = 0, v = 0, b = 0;
            int ra = raw_gettime(id, &a);
            int rv = vdso_gettime(id, &v);
            int rb = raw_gettime(id, &b);
            if (ra != 0 || rv != 0 || rb != 0) {
                printf("FAIL %s: raw %d, vdso %d, raw %d\n", served[c].name, ra, rv, rb);
                failures_total++;
                break;
            }
            if (v < a || v > b) {
                if (bad++ < 3)
                    printf("FAIL %s: vdso read %lld not between the system call's "
                           "%lld and %lld\n", served[c].name, (long long) v,
                           (long long) a, (long long) b);
            }
            if (v - a > worst_late)
                worst_late = v - a;
            if (b - v > worst_early)
                worst_early = b - v;
        }
        if (bad != 0) {
            printf("FAIL %s: %u of 3000 vdso reads outside the bracket\n",
                   served[c].name, bad);
            failures_total++;
        }
        test_logf("%-17s bracketed; widest gap after the first read %lld ns, "
                  "before the second %lld ns\n", served[c].name,
                  (long long) worst_late, (long long) worst_early);
    }
}

// ---- 3. what it leaves to the kernel comes back as the kernel says --------

static void check_fallbacks(void) {
    clockid_t cpu_ids[3] = {CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID, 0};
    int n_cpu = 2;
    clockid_t dyn;
    if (clock_getcpuclockid(0, &dyn) == 0)
        cpu_ids[n_cpu++] = dyn;
    for (int i = 0; i < n_cpu; i++) {
        int64_t a = 0, v = 0;
        int ra = raw_gettime(cpu_ids[i], &a);
        int rv = vdso_gettime(cpu_ids[i], &v);
        if (ra != 0 || rv != 0 || v <= 0) {
            printf("FAIL cpu clock %d: raw %d, vdso %d, reading %lld\n",
                   (int) cpu_ids[i], ra, rv, (long long) v);
            failures_total++;
        }
    }
    // Ids Linux does not have: the vDSO returns the raw -errno.
    clockid_t bogus[] = {12345, 16, -1};
    for (size_t i = 0; i < sizeof(bogus) / sizeof(bogus[0]); i++) {
        int64_t v = 0, a = 0;
        int ra = raw_gettime(bogus[i], &a);
        int rv = vdso_gettime(bogus[i], &v);
        if (ra != -EINVAL || rv != -EINVAL) {
            printf("FAIL clock %d: raw %d, vdso %d, want both -EINVAL (%d)\n",
                   (int) bogus[i], ra, rv, -EINVAL);
            failures_total++;
        }
    }
    // clock_getres: the same answer both ways, for everything above.
    if (vdso_getres_fn != NULL) {
        for (size_t c = 0; c < sizeof(served) / sizeof(served[0]); c++) {
            int64_t a = -2, v = -3;
            int ra = raw_getres(served[c].id, &a);
            int rv = vdso_getres(served[c].id, &v);
            if (ra != 0 || rv != 0 || a != v) {
                printf("FAIL clock_getres(%s): raw %d %lld, vdso %d %lld\n",
                       served[c].name, ra, (long long) a, rv, (long long) v);
                failures_total++;
            }
        }
        int64_t v = 0;
        int rv = vdso_getres(12345, &v);
        if (rv != -EINVAL) {
            printf("FAIL clock_getres(12345) through the vDSO: %d, want -EINVAL\n", rv);
            failures_total++;
        }
    }
}

// ---- 4. gettimeofday ------------------------------------------------------

static void check_gettimeofday(void) {
    if (vdso_gettod_fn == NULL)
        return;
    unsigned bad = 0;
    for (int i = 0; i < 2000; i++) {
        struct ktv tv = {-1, -1};
        int64_t a = 0, b = 0;
        raw_gettime(CLOCK_REALTIME, &a);
        int r = vdso_gettod(&tv, NULL);
        raw_gettime(CLOCK_REALTIME, &b);
        int64_t v = (int64_t) tv.sec * 1000000000 + (int64_t) tv.usec * 1000;
        // Microseconds: the reading is truncated, so it may sit up to 1 us
        // below the first raw read, never above the second.
        if (r != 0 || tv.usec < 0 || tv.usec >= 1000000 ||
                v < a - (a % 1000) || v > b) {
            if (bad++ < 3)
                printf("FAIL gettimeofday: %d {%ld, %ld} against %lld..%lld\n",
                       r, tv.sec, tv.usec, (long long) a, (long long) b);
        }
    }
    if (bad != 0) {
        printf("FAIL gettimeofday: %u of 2000 reads outside the bracket\n", bad);
        failures_total++;
    }
    // A timezone: Linux's is {0, 0}.
    struct { int minuteswest, dsttime; } tz = {77, 77};
    struct ktv tv = {0, 0};
    int r = vdso_gettod(&tv, &tz);
    if (r != 0 || tz.minuteswest != 0 || tz.dsttime != 0 || tv.sec == 0) {
        printf("FAIL gettimeofday with a timezone: %d {%d, %d} sec %ld\n",
               r, tz.minuteswest, tz.dsttime, tv.sec);
        failures_total++;
    }
    tz.minuteswest = tz.dsttime = 77;
    r = vdso_gettod(NULL, &tz);
    if (r != 0 || tz.minuteswest != 0 || tz.dsttime != 0) {
        printf("FAIL gettimeofday(NULL, tz): %d {%d, %d}\n", r, tz.minuteswest, tz.dsttime);
        failures_total++;
    }
    if ((r = vdso_gettod(NULL, NULL)) != 0) {
        printf("FAIL gettimeofday(NULL, NULL): %d\n", r);
        failures_total++;
    }
}

// ---- 5. a file's mtime is never in the future ----------------------------

static void check_mtimes_in(const char *dir) {
    char path[256];
    snprintf(path, sizeof(path), "%s/vdso_clock.%d", dir, (int) getpid());
    unsigned future = 0;
    int64_t worst = 0;
    for (int i = 0; i < 300; i++) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            printf("FAIL open %s: %s\n", path, strerror(errno));
            failures_total++;
            return;
        }
        if (write(fd, "x", 1) != 1) {
            printf("FAIL write %s: %s\n", path, strerror(errno));
            failures_total++;
        }
        // Every other time the stamp comes from utimensat's UTIME_NOW instead.
        if (i % 2 == 1 && futimens(fd, NULL) != 0) {
            printf("FAIL futimens %s: %s\n", path, strerror(errno));
            failures_total++;
        }
        int64_t now = 0;
        vdso_gettime(CLOCK_REALTIME, &now);
        struct stat st;
        fstat(fd, &st);
        close(fd);
        int64_t m = (int64_t) st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
        int64_t c = (int64_t) st.st_ctim.tv_sec * 1000000000 + st.st_ctim.tv_nsec;
        if (m > now || c > now) {
            future++;
            if (m - now > worst)
                worst = m - now;
            if (c - now > worst)
                worst = c - now;
        }
    }
    unlink(path);
    if (future != 0) {
        printf("FAIL %s: %u of 300 files stamped after a later vDSO read, "
               "by up to %lld ns\n", dir, future, (long long) worst);
        failures_total++;
    }
    test_logf("mtimes in %s: none later than the vDSO read that followed\n", dir);
}

static void check_mtimes(void) {
    check_mtimes_in(".");
    // And a tmpfs, where the kernel writes the stamp itself: one of ours when
    // we may mount it, else whichever the system has.
    char dir[] = "/tmp/vdso_clock.XXXXXX";
    if (mkdtemp(dir) != NULL) {
        if (mount("tmpfs", dir, "tmpfs", 0, "size=1m") == 0) {
            check_mtimes_in(dir);
            if (umount(dir) != 0) {
                printf("FAIL umount %s: %s\n", dir, strerror(errno));
                failures_total++;
            }
            rmdir(dir);
            return;
        }
        rmdir(dir);
    }
    const char *tmpfs[] = {"/dev/shm", "/tmp", "/run"};
    for (size_t i = 0; i < sizeof(tmpfs) / sizeof(tmpfs[0]); i++) {
        struct statfs sf;
        if (statfs(tmpfs[i], &sf) == 0 && sf.f_type == 0x01021994 &&
                access(tmpfs[i], W_OK) == 0) {
            check_mtimes_in(tmpfs[i]);
            return;
        }
    }
    printf("vdso_clock: no tmpfs to write to; tmpfs mtimes not checked\n");
}

// ---- 6. MONOTONIC never goes backward --------------------------------------

static _Atomic int64_t shared_last;
static _Atomic unsigned backward;

static void *monotonic_worker(void *arg) {
    (void) arg;
    int64_t mine = 0;
    for (int i = 0; i < 20000; i++) {
        // Read the shared value BEFORE the clock: whatever it holds was read
        // earlier than this read, so this one may not be smaller.
        int64_t prev = atomic_load(&shared_last);
        int64_t v = 0;
        if (vdso_gettime(CLOCK_MONOTONIC, &v) != 0 || v < prev || v < mine)
            atomic_fetch_add(&backward, 1);
        mine = v;
        int64_t cur = atomic_load(&shared_last);
        while (cur < v && !atomic_compare_exchange_weak(&shared_last, &cur, v))
            ;
    }
    return NULL;
}

static void check_monotonic_threads(void) {
    pthread_t t[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, monotonic_worker, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    if (atomic_load(&backward) != 0) {
        printf("FAIL CLOCK_MONOTONIC went backward %u times across 4 threads\n",
               atomic_load(&backward));
        failures_total++;
    }
}

// ---- 7. not a system call --------------------------------------------------

static int install_clock_filter(void) {
    struct sock_filter prog[2 + 2 * 8 + 1];
    unsigned n = 0, k = sizeof(gettime_nrs) / sizeof(gettime_nrs[0]);
    prog[n++] = (struct sock_filter) BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                              offsetof(struct seccomp_data, nr));
    for (unsigned i = 0; i < k; i++) {
        prog[n++] = (struct sock_filter) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                                  (unsigned) gettime_nrs[i], 0, 1);
        prog[n++] = (struct sock_filter) BPF_STMT(BPF_RET | BPF_K,
                                                  SECCOMP_RET_ERRNO | EPERM);
    }
    prog[n++] = (struct sock_filter) BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    struct sock_fprog fprog = {.len = (unsigned short) n, .filter = prog};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return -1;
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog, 0, 0);
}

static void check_seccomp(void) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (install_clock_filter() != 0) {
            printf("FAIL could not install the seccomp filter: %s\n", strerror(errno));
            _exit(1);
        }
        int fails = 0;
        // The control: the raw calls are refused.
        int64_t v = 0;
        int r = raw_gettime(CLOCK_MONOTONIC, &v);
        if (r != -EPERM) {
            printf("FAIL control: raw clock_gettime under the filter gave %d, want -EPERM\n", r);
            fails++;
        }
        // The vDSO and the C library still read the clock.
        if ((r = vdso_gettime(CLOCK_MONOTONIC, &v)) != 0 || v <= 0) {
            printf("FAIL the vDSO's clock_gettime under the filter: %d\n", r);
            fails++;
        }
        if (libc_uses_vdso()) {
            struct timespec ts = {0, 0};
            if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec == 0) {
                printf("FAIL the C library's clock_gettime under the filter: %s\n",
                       strerror(errno));
                fails++;
            }
            struct timeval tv = {0, 0};
            if (gettimeofday(&tv, NULL) != 0 || tv.tv_sec == 0) {
                printf("FAIL the C library's gettimeofday under the filter: %s\n",
                       strerror(errno));
                fails++;
            }
        } else {
            printf("vdso_clock: this C library does not use the vDSO (musl on "
                   "riscv64); its own clock_gettime not checked\n");
        }
        // A CPU clock is still the kernel's, so it is refused too.
        if ((r = vdso_gettime(CLOCK_PROCESS_CPUTIME_ID, &v)) != -EPERM) {
            printf("FAIL CLOCK_PROCESS_CPUTIME_ID through the vDSO under the "
                   "filter: %d, want -EPERM\n", r);
            fails++;
        }
        fflush(stdout);
        _exit(fails);
    }
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL seccomp child: status %#x\n", status);
        failures_total++;
    }
}

// Run `reads` clock reads in a traced child -- vDSO ones, or raw ones -- and
// return how many syscall stops the tracer saw from the child's first stop to
// its exit.
static long traced_stops(int reads, int raw) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        int64_t v;
        for (int i = 0; i < reads; i++) {
            if (raw)
                raw_gettime(CLOCK_MONOTONIC, &v);
            else
                vdso_gettime(CLOCK_MONOTONIC, &v);
        }
        _exit(0);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status))
        return -1;
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *) (long) PTRACE_O_TRACESYSGOOD);
    long stops = 0;
    for (;;) {
        if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) != 0)
            return -1;
        if (waitpid(pid, &status, 0) != pid)
            return -1;
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break;
        if (WIFSTOPPED(status) && WSTOPSIG(status) == (SIGTRAP | 0x80))
            stops++;
    }
    return stops;
}

static void check_ptrace(void) {
    long none = traced_stops(0, 0);
    long vdso = traced_stops(50, 0);
    long raw = traced_stops(50, 1);
    test_logf("syscall stops: baseline %ld, 50 vDSO reads %ld, 50 raw reads %ld\n",
              none, vdso, raw);
    if (none < 0 || vdso < 0 || raw < 0) {
        printf("FAIL tracing the children: %ld %ld %ld\n", none, vdso, raw);
        failures_total++;
        return;
    }
    // The control: 50 raw calls are 100 stops, an entry and an exit each.
    if (raw - none != 100) {
        printf("FAIL control: 50 raw clock_gettime calls made %ld syscall stops, "
               "want 100\n", raw - none);
        failures_total++;
    }
    if (vdso != none) {
        printf("FAIL 50 vDSO clock reads made %ld syscall stops\n", vdso - none);
        failures_total++;
    }
}

// ---- 8. each process has its own ------------------------------------------

// The size of the vDSO's image, from its one PT_LOAD, in whole pages.
static size_t vdso_image_size(void) {
    Ehdr *eh = (Ehdr *) getauxval(AT_SYSINFO_EHDR);
    Phdr *ph = (Phdr *) ((char *) eh + eh->e_phoff);
    size_t size = 0;
    for (int i = 0; i < eh->e_phnum; i++, ph = (Phdr *) ((char *) ph + eh->e_phentsize))
        if (ph->p_type == PT_LOAD && ph->p_vaddr + ph->p_memsz > size)
            size = ph->p_vaddr + ph->p_memsz;
    return (size + 4095) & ~(size_t) 4095;
}

// Write one byte of read-only code in one of the two ways a process can: the
// way a debugger plants a breakpoint, through /proc/self/mem, which may write
// a read-only page; or by making the whole mapping [base, base + size)
// writable and storing to it (the WHOLE mapping: Linux refuses to split its
// vDSO with a partial mprotect, EINVAL). 'y' when the byte took.
enum patch_how { PATCH_PROC_MEM, PATCH_MPROTECT };
static const char *const patch_how_name[] = {"/proc/self/mem", "mprotect"};

static char patch_code_byte(enum patch_how how, volatile unsigned char *at,
                            unsigned char value, void *base, size_t size) {
    if (how == PATCH_PROC_MEM) {
        int fd = open("/proc/self/mem", O_RDWR);
        if (fd < 0)
            return 'n';
        ssize_t n = pwrite(fd, &value, 1, (off_t) (uintptr_t) at);
        close(fd);
        return n == 1 && *at == value ? 'y' : 'n';
    }
    if (mprotect(base, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return 'n';
    *at = value;
    return *at == value ? 'y' : 'n';
}

// Whether this process's /proc/self/maps still shows the whole vDSO as one
// [vdso] line. A patch makes a page of it private, and Linux keeps the one
// line; AOK's private copy of the page used to lose the name, so the line
// shrank (i386) or vanished (arm64) -- and the line's extent is what gdb
// reads the vDSO by.
static int vdso_maps_whole(void) {
    uintptr_t base = (uintptr_t) getauxval(AT_SYSINFO_EHDR);
    uintptr_t end = base + vdso_image_size();
    FILE *f = fopen("/proc/self/maps", "r");
    if (f == NULL)
        return 0;
    char line[512];
    int whole = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && lo <= base && hi >= end &&
                strstr(line, "[vdso]") != NULL)
            whole = 1;
    }
    fclose(f);
    return whole;
}

// A forked child patches one byte at `code` and holds it while its parent
// looks, then puts it back. Returns whether the parent saw the child's byte;
// *took says whether the child managed to patch at all, and is 2 when it did
// but a patched vDSO no longer showed whole in its maps (vdso_maps_whole).
static int child_patch_seen(enum patch_how how, volatile unsigned char *code,
                            void *base, size_t size, int *took) {
    unsigned char before = *code;
    int patched[2], looked[2];
    if (pipe(patched) != 0 || pipe(looked) != 0) {
        printf("FAIL pipe: %s\n", strerror(errno));
        failures_total++;
        *took = 0;
        return 0;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        char c = patch_code_byte(how, code, (unsigned char) (before ^ 0xff), base, size);
        if (c == 'y' && base == (void *) getauxval(AT_SYSINFO_EHDR) && !vdso_maps_whole())
            c = 'm';
        (void) !write(patched[1], &c, 1);
        (void) !read(looked[0], &c, 1);
        patch_code_byte(how, code, before, base, size);
        _exit(0);
    }
    char c = 0;
    (void) !read(patched[0], &c, 1);
    unsigned char during = *code;
    (void) !write(looked[1], "k", 1);
    int status;
    waitpid(pid, &status, 0);
    close(patched[0]); close(patched[1]); close(looked[0]); close(looked[1]);
    *took = c == 'y' ? 1 : c == 'm' ? 2 : 0;
    test_logf("%s: a forked child patched %p (%s); its parent's byte %#x, during %#x\n",
              patch_how_name[how], (void *) code,
              c == 'y' ? "took" : c == 'm' ? "took; maps lost the vDSO" : "refused",
              before, during);
    return during != before;
}

// The same between two processes that share nothing but the kernel: a freshly
// exec'd one -- which has never forked, so none of its pages is copy-on-write
// -- patches its own vDSO and runs a second fresh one, which reports the byte
// it finds at the same offset of ITS vDSO. `--peek` and `--patch` are those
// two, below. (A forked relative cannot see this: fork makes the page
// copy-on-write, and the store then breaks it.) With `file`, the patcher
// patches a MAP_SHARED mapping of that file instead and the peeker maps the
// same file: the control, which must be seen.
static int peek_mode(int argc, char **argv) {
    if (argc < 3)
        return 255;
    size_t off = strtoul(argv[2], NULL, 0);
    if (argc >= 4) {
        int fd = open(argv[3], O_RDONLY);
        unsigned char *p = fd < 0 ? MAP_FAILED :
                mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
        return p == MAP_FAILED ? 254 : p[off];
    }
    return ((volatile unsigned char *) getauxval(AT_SYSINFO_EHDR))[off];
}

static int patch_mode(int argc, char **argv) {
    // argv: --patch HOW OFF [FILE]; exit 0 private, 1 seen, 2 refused, 3 error
    if (argc < 4)
        return 3;
    enum patch_how how = (enum patch_how) atoi(argv[2]);
    size_t off = strtoul(argv[3], NULL, 0);
    void *base;
    size_t size;
    if (argc >= 5) {
        int fd = open(argv[4], O_RDWR);
        base = fd < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED)
            return 3;
        size = 4096;
    } else {
        base = (void *) getauxval(AT_SYSINFO_EHDR);
        size = vdso_image_size();
    }
    volatile unsigned char *at = (volatile unsigned char *) base + off;
    unsigned char before = *at;
    if (patch_code_byte(how, at, (unsigned char) (before ^ 0xff), base, size) != 'y')
        return 2;
    int maps_lost = argc < 5 && !vdso_maps_whole();
    char offs[32];
    snprintf(offs, sizeof(offs), "%#zx", off);
    pid_t pid = fork();
    if (pid == 0) {
        if (argc >= 5)
            execl("/proc/self/exe", "vdso_clock", "--peek", offs, argv[4], (char *) NULL);
        else
            execl("/proc/self/exe", "vdso_clock", "--peek", offs, (char *) NULL);
        _exit(255);
    }
    int status;
    waitpid(pid, &status, 0);
    patch_code_byte(how, at, before, base, size);
    if (!WIFEXITED(status) || WEXITSTATUS(status) >= 254)
        return 3;
    if (WEXITSTATUS(status) == (unsigned char) (before ^ 0xff))
        return 1;
    return maps_lost ? 4 : 0;
}

// 0 private, 1 seen, 2 refused, 3 error, 4 private but the patcher's maps lost
// the vDSO: patch_mode's answer.
static int fresh_patch_seen(enum patch_how how, size_t off, const char *file) {
    char hows[8], offs[32];
    snprintf(hows, sizeof(hows), "%d", (int) how);
    snprintf(offs, sizeof(offs), "%#zx", off);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (file != NULL)
            execl("/proc/self/exe", "vdso_clock", "--patch", hows, offs, file, (char *) NULL);
        else
            execl("/proc/self/exe", "vdso_clock", "--patch", hows, offs, (char *) NULL);
        _exit(3);
    }
    int status;
    waitpid(pid, &status, 0);
    int r = WIFEXITED(status) ? WEXITSTATUS(status) : 3;
    test_logf("%s: a fresh process patched %s at %#zx: %s\n", patch_how_name[how],
              file != NULL ? file : "its vDSO", off,
              r == 0 ? "a second fresh one saw the original" :
              r == 1 ? "a second fresh one SAW THE PATCH" :
              r == 2 ? "refused" : r == 4 ? "its own maps lost the vDSO" : "error");
    return r;
}

static void check_private_copy(void) {
    void *base = (void *) getauxval(AT_SYSINFO_EHDR);
    size_t off = (size_t) ((char *) vdso_gettime_fn - (char *) base);
    int took;

    // Between a parent and its forked child. The control is a MAP_SHARED
    // page, which a child's patch must reach. Only for the mprotect route:
    // Linux refuses /proc/self/mem writes to a shared mapping outright (a
    // forced write may only break copy-on-write), so the other route has no
    // shared page to control against.
    void *shared = mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        printf("FAIL mmap of the control page: %s\n", strerror(errno));
        failures_total++;
    } else {
        if (!child_patch_seen(PATCH_MPROTECT, (volatile unsigned char *) shared, shared,
                              4096, &took) || !took) {
            printf("FAIL control: a forked child's patch to a MAP_SHARED page was %s\n",
                   took ? "not seen by its parent" : "refused");
            failures_total++;
        }
        munmap(shared, 4096);
    }
    for (int how = PATCH_PROC_MEM; how <= PATCH_MPROTECT; how++) {
        int seen = child_patch_seen(how, (volatile unsigned char *) vdso_gettime_fn,
                                    base, vdso_image_size(), &took);
        if (!took) {
            printf("FAIL a forked child could not patch its vDSO through %s\n",
                   patch_how_name[how]);
            failures_total++;
        } else if (took == 2) {
            printf("FAIL after a %s patch, /proc/self/maps no longer shows the whole "
                   "vDSO as [vdso]\n", patch_how_name[how]);
            failures_total++;
        }
        if (took && seen) {
            printf("FAIL a forked child's %s patch to its vDSO changed its parent's\n",
                   patch_how_name[how]);
            failures_total++;
        }
    }

    // Between unrelated fresh processes. The control: a MAP_SHARED mapping of
    // one file, patched the same way, which the second process must see.
    char file[] = "vdso_clock.shared.XXXXXX";
    int fd = mkstemp(file);
    // Not zero: a patched zero is 0xff, which --peek's exit status reserves.
    unsigned char page[4096];
    memset(page, 0x11, sizeof(page));
    if (fd < 0 || write(fd, page, sizeof(page)) != (ssize_t) sizeof(page)) {
        printf("FAIL control file: %s\n", strerror(errno));
        failures_total++;
    } else {
        close(fd);
        int r = fresh_patch_seen(PATCH_MPROTECT, 100, file);
        if (r != 1) {
            printf("FAIL control: a fresh process's patch to a shared file mapping, "
                   "seen by another: %d, want 1\n", r);
            failures_total++;
        }
    }
    if (fd >= 0)
        unlink(file);
    for (int how = PATCH_PROC_MEM; how <= PATCH_MPROTECT; how++) {
        int r = fresh_patch_seen(how, off, NULL);
        if (r == 1) {
            printf("FAIL a process's %s patch to its vDSO reached another process's "
                   "vDSO -- any process could rewrite the clock_gettime every other "
                   "one runs\n", patch_how_name[how]);
            failures_total++;
        } else if (r == 4) {
            printf("FAIL after a %s patch, a fresh process's /proc/self/maps no longer "
                   "shows the whole vDSO as [vdso]\n", patch_how_name[how]);
            failures_total++;
        } else if (r != 0) {
            printf("FAIL a fresh process's %s patch to its vDSO: %s\n", patch_how_name[how],
                   r == 2 ? "refused" : "error");
            failures_total++;
        }
    }

    // And a fresh process after all that still gets a working one.
    fflush(stdout);
    int status;
    pid_t pid = fork();
    if (pid == 0) {
        execl("/proc/self/exe", "vdso_clock", "--probe", (char *) NULL);
        _exit(127);
    }
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL a process exec'd after the patches: status %#x\n", status);
        failures_total++;
    }
}

// The exec'd probe: its own vDSO reads the clock.
static int probe(void) {
    if (!check_symbols())
        return 1;
    int64_t a = 0, v = 0, b = 0;
    raw_gettime(CLOCK_MONOTONIC, &a);
    int r = vdso_gettime(CLOCK_MONOTONIC, &v);
    raw_gettime(CLOCK_MONOTONIC, &b);
    return r == 0 && a <= v && v <= b ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--probe") == 0)
        return probe();
    if (argc >= 2 && strcmp(argv[1], "--peek") == 0)
        return peek_mode(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--patch") == 0)
        return patch_mode(argc, argv);
    test_init(argc, argv);
    alarm(test_watchdog_secs(300));
    if (check_symbols()) {
        check_brackets();
        check_fallbacks();
        check_gettimeofday();
        check_mtimes();
        check_monotonic_threads();
        if (vdso_makes_syscalls()) {
            // Not "vdso_clock: SKIP": the runner would take that for the
            // verdict, and then not report a crash that came after it.
            printf("vdso_clock: not checking seccomp and ptrace -- AOK's i386 "
                   "vDSO makes the system call (vdso/vdso.c)\n");
        } else {
            check_seccomp();
            check_ptrace();
        }
        check_private_copy();
    }
    return finish_suite(NAME);
}
