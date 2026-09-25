// A forced write -- what a debugger does to plant a breakpoint, through
// PTRACE_POKETEXT or /proc/<pid>/mem -- into a page the process may not write.
//
// Linux (FOLL_FORCE) bypasses the permission check for that one access and
// grants nothing: a private page is copied (copy-on-write) and the byte lands
// in the copy, which keeps the mapping's protection and its place in the
// mapping, so /proc/<pid>/maps shows the same line before and after and the
// process's own store to the page still faults. A read-only SHARED page is
// refused (EIO), since the byte would reach every other mapper and the file.
//
// AOK used to leave the page writable: the poke added P_WRITE (and P_COW) to
// the entry, and the copy-on-write break mapped the copy with those flags, so
// after one gdb breakpoint the program could store to its own code with no
// mprotect, maps showed the page `rwxp`, and -- since the copy was a new
// struct data with no fd and no name -- nameless, splitting the file's line.
//
// MEASURED on Linux 6.12 (camd, x86_64 and -m32), the facts asserted below:
//  - text page, never forked, /proc/self/mem: the maps line holding the
//    address is byte-identical after a write, the process sees the byte, the
//    page stays r-xp and a store to it is SIGSEGV/SEGV_ACCERR;
//  - the same through PTRACE_POKETEXT into a freshly exec'd tracee (gdb's
//    shape: never forked, so nothing is copy-on-write) and into a forked one;
//  - the vDSO the same way, and it stays one [vdso] line;
//  - a 4-page private file mapping: one line for all 4 pages after a forced
//    write to the second, the file untouched, and an mprotect split after it
//    prints each piece at its own file offset;
//  - an ordinary copy-on-write break (a store after fork) of a writable
//    private file page keeps the file's line too;
//  - PROT_NONE: a forced write takes and the page stays ---p; a forced read
//    reads it, zeroes where nothing was ever written;
//  - [vvar] (VM_IO|VM_PFNMAP): a forced read or write is EIO;
//  - a read-only MAP_SHARED page: the write is refused with EIO and neither
//    the file nor the line changes; a writable one takes, and reaches the file;
//  - mlock survives the copy, and a store's copy after fork: RLIMIT_MEMLOCK
//    still counts the page;
//  - mremap grows a file mapping whose front was unmapped from the offset
//    after its own last page;
//  - pokes after the first go in place (50 of them, 0 minor faults);
//  - smaps counts a forced write's copy as Anonymous and Private_Dirty;
//  - past the end of a file: a forced read or write is EIO, and a store that
//    has to copy the page after a fork is SIGBUS;
//  - PTRACE_POKEDATA/PEEKDATA and /proc/self/mem at an unmapped address: EIO.

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

_Static_assert(sizeof(off_t) == 8, "the /proc/self/mem offsets need a 64-bit off_t");

#define PG 4096ul

static void check(const char *what, int ok) {
    if (!ok) {
        printf("FAIL %s\n", what);
        failures_total++;
    } else {
        test_logf("ok   %s\n", what);
    }
}

// Never called: its first byte is what the tests patch, so nothing may run it.
__attribute__((noinline, used)) int victim_fn(int x) {
    return x * 7 + 3;
}

// Called after the patches, to show the page still runs.
__attribute__((noinline)) int still_runs(int x) {
    return x * 5 + 1;
}

// ---- /proc/<pid>/maps ------------------------------------------------------

// The maps line whose range holds `addr`, newline stripped; 0 when none.
static int maps_line(pid_t pid, uintptr_t addr, char *out, size_t size) {
    char path[64];
    if (pid == 0)
        snprintf(path, sizeof path, "/proc/self/maps");
    else
        snprintf(path, sizeof path, "/proc/%d/maps", (int) pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && addr >= lo && addr < hi) {
            line[strcspn(line, "\n")] = '\0';
            snprintf(out, size, "%s", line);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

// The fields of a maps line this test compares: its extent, permissions,
// file offset and path (the device and inode are Linux's own, and AOK's 0s).
struct maps_fields {
    unsigned long lo, hi, off;
    char perms[5];
    char path[512];
};

static int maps_parse(const char *line, struct maps_fields *m) {
    int n = 0;
    memset(m, 0, sizeof *m);
    if (sscanf(line, "%lx-%lx %4s %lx %*s %*s %n", &m->lo, &m->hi, m->perms, &m->off, &n) < 4)
        return 0;
    if (n > 0)
        snprintf(m->path, sizeof m->path, "%s", line + n);
    return 1;
}

static int maps_fields_at(pid_t pid, uintptr_t addr, struct maps_fields *m) {
    char line[1024];
    return maps_line(pid, addr, line, sizeof line) && maps_parse(line, m);
}

// ---- stores that should fault -----------------------------------------------

static sigjmp_buf segv_jmp;
static volatile int segv_code;
static void *volatile segv_addr;

static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void) sig;
    (void) uc;
    segv_code = si->si_code;
    segv_addr = si->si_addr;
    siglongjmp(segv_jmp, 1);
}

// 1 when a store to `p` (of the byte already there) faults SEGV_ACCERR at p;
// 0 when it goes through; -1 when it faults some other way.
static int store_faults(volatile unsigned char *p) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old);
    unsigned char v = *p;
    int r;
    segv_code = 0;
    segv_addr = NULL;
    if (sigsetjmp(segv_jmp, 1) == 0) {
        *p = v;
        r = 0;
    } else {
        r = segv_code == SEGV_ACCERR && segv_addr == (void *) p ? 1 : -1;
    }
    sigaction(SIGSEGV, &old, NULL);
    if (r < 0)
        test_logf("store to %p faulted with si_code %d at %p\n", (void *) p, segv_code, segv_addr);
    return r;
}

// Same for a load (of a PROT_NONE page).
static int load_faults(volatile unsigned char *p) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old);
    int r;
    segv_code = 0;
    segv_addr = NULL;
    if (sigsetjmp(segv_jmp, 1) == 0) {
        (void) *p;
        r = 0;
    } else {
        r = segv_code == SEGV_ACCERR && segv_addr == (void *) p ? 1 : -1;
    }
    sigaction(SIGSEGV, &old, NULL);
    return r;
}

// ---- /proc/self/mem ----------------------------------------------------------

static int mem_fd = -1;

// One byte through /proc/self/mem: 0, or the errno.
static int mem_write(uintptr_t addr, unsigned char v) {
    errno = 0;
    ssize_t n = pwrite(mem_fd, &v, 1, (off_t) addr);
    return n == 1 ? 0 : errno != 0 ? errno : -1;
}

static int mem_read(uintptr_t addr, unsigned char *v) {
    errno = 0;
    ssize_t n = pread(mem_fd, v, 1, (off_t) addr);
    return n == 1 ? 0 : errno != 0 ? errno : -1;
}

// A forced write of the byte already at `addr`, then of its complement (the
// process must see it), then back: the maps line holding `addr` must not move
// at any point, and the process's own store to it must still fault.
static void forced_write_readonly(const char *what, uintptr_t addr, const char *perms) {
    char label[2400], before[1024], after[1024];
    volatile unsigned char *p = (volatile unsigned char *) addr;
    snprintf(label, sizeof label, "%s: has a maps line", what);
    check(label, maps_line(0, addr, before, sizeof before));
    struct maps_fields mf;
    snprintf(label, sizeof label, "%s: is %s before the write", what, perms);
    check(label, maps_parse(before, &mf) && strcmp(mf.perms, perms) == 0);

    unsigned char orig = *p;
    snprintf(label, sizeof label, "%s: /proc/self/mem writes the same byte back", what);
    check(label, mem_write(addr, orig) == 0);
    after[0] = '\0';
    maps_line(0, addr, after, sizeof after);
    snprintf(label, sizeof label, "%s: maps line unchanged by it (before [%s] after [%s])",
             what, before, after);
    check(label, strcmp(before, after) == 0);

    snprintf(label, sizeof label, "%s: /proc/self/mem writes a new byte", what);
    check(label, mem_write(addr, (unsigned char) (orig ^ 0xff)) == 0);
    snprintf(label, sizeof label, "%s: the process sees the new byte", what);
    check(label, *p == (unsigned char) (orig ^ 0xff));
    unsigned char back = 0;
    snprintf(label, sizeof label, "%s: /proc/self/mem reads the new byte", what);
    check(label, mem_read(addr, &back) == 0 && back == (unsigned char) (orig ^ 0xff));
    snprintf(label, sizeof label, "%s: /proc/self/mem puts the byte back", what);
    check(label, mem_write(addr, orig) == 0 && *p == orig);
    after[0] = '\0';
    maps_line(0, addr, after, sizeof after);
    snprintf(label, sizeof label, "%s: maps line still unchanged (before [%s] after [%s])",
             what, before, after);
    check(label, strcmp(before, after) == 0);

    snprintf(label, sizeof label, "%s: the process's own store still faults (SEGV_ACCERR)", what);
    check(label, store_faults(p) == 1);
}

// [vvar], where there is one, is out of a debugger's reach: Linux maps it
// VM_IO|VM_PFNMAP, and a forced read or write of it is EIO.
static void forced_access_vvar(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (f == NULL)
        return;
    char line[512];
    unsigned long lo[4];
    int n = 0;
    while (n < 4 && fgets(line, sizeof line, f) != NULL) {
        unsigned long l, h;
        if (strstr(line, "[vvar") != NULL && sscanf(line, "%lx-%lx", &l, &h) == 2)
            lo[n++] = l;
    }
    fclose(f);
    if (n == 0)
        test_logf("no [vvar]; its forced access is not checked\n");
    for (int i = 0; i < n; i++) {
        char before[1024], after[1024], label[2400];
        before[0] = after[0] = '\0';
        maps_line(0, lo[i], before, sizeof before);
        unsigned char b = 0;
        int re = mem_read(lo[i], &b);
        int we = mem_write(lo[i], 0);
        maps_line(0, lo[i], after, sizeof after);
        snprintf(label, sizeof label, "[vvar] at %#lx: a forced read is EIO (got %d)", lo[i], re);
        check(label, re == EIO);
        snprintf(label, sizeof label, "[vvar] at %#lx: a forced write is EIO (got %d)", lo[i], we);
        check(label, we == EIO);
        snprintf(label, sizeof label, "[vvar] at %#lx: maps line unchanged (before [%s] after [%s])",
                 lo[i], before, after);
        check(label, strcmp(before, after) == 0);
    }
}

// ---- the tracee side ----------------------------------------------------------

// argv: --tracee FD. Tells the tracer where its victim byte and vDSO are,
// stops, and once resumed checks what the tracer's pokes left: exit status is
// a bitmask of what was wrong, 0 when nothing was.
enum {
    TR_TEXT_UNSEEN = 1,     // the poked text byte is not what the tracer wrote
    TR_TEXT_STORE = 2,      // a store to the text page did not fault
    TR_VDSO_UNSEEN = 4,
    TR_VDSO_STORE = 8,
    TR_TEXT_RUNS = 16,      // the text page no longer runs
};

struct tracee_report {
    uint64_t text, vdso;
    unsigned char text_byte, vdso_byte;
};

static int tracee_main(int fd) {
    struct tracee_report r;
    memset(&r, 0, sizeof r);
    r.text = (uintptr_t) &victim_fn;
    r.vdso = (uintptr_t) getauxval(AT_SYSINFO_EHDR);
    r.text_byte = *(volatile unsigned char *) (uintptr_t) r.text;
    if (r.vdso != 0)
        r.vdso_byte = *(volatile unsigned char *) (uintptr_t) (r.vdso + 9);
    if (write(fd, &r, sizeof r) != (ssize_t) sizeof r)
        return 100;
    close(fd);
    raise(SIGSTOP);
    // The tracer flipped each byte and left it flipped.
    int bad = 0;
    volatile unsigned char *t = (volatile unsigned char *) (uintptr_t) r.text;
    if (*t != (unsigned char) (r.text_byte ^ 0xff))
        bad |= TR_TEXT_UNSEEN;
    if (store_faults(t) != 1)
        bad |= TR_TEXT_STORE;
    if (still_runs(3) != 16)
        bad |= TR_TEXT_RUNS;
    if (r.vdso != 0) {
        volatile unsigned char *v = (volatile unsigned char *) (uintptr_t) (r.vdso + 9);
        if (*v != (unsigned char) (r.vdso_byte ^ 0xff))
            bad |= TR_VDSO_UNSEEN;
        if (store_faults(v) != 1)
            bad |= TR_VDSO_STORE;
    }
    return bad;
}

// PEEKTEXT, flip the low-address byte of the word, POKETEXT, PEEKTEXT again;
// the tracee's maps line holding `addr` must be the same before and after.
static void poke_flip(const char *what, pid_t pid, uintptr_t addr, unsigned char expect_byte) {
    char label[2400], before[1024], after[1024];
    snprintf(label, sizeof label, "%s: tracee has a maps line", what);
    check(label, maps_line(pid, addr, before, sizeof before));
    errno = 0;
    long word = ptrace(PTRACE_PEEKTEXT, pid, (void *) addr, NULL);
    snprintf(label, sizeof label, "%s: PEEKTEXT", what);
    check(label, errno == 0 && (unsigned char) word == expect_byte);
    long flipped = word ^ 0xff;   // the byte AT addr: every guest is little-endian
    snprintf(label, sizeof label, "%s: POKETEXT", what);
    check(label, ptrace(PTRACE_POKETEXT, pid, (void *) addr, (void *) flipped) == 0);
    errno = 0;
    long now = ptrace(PTRACE_PEEKTEXT, pid, (void *) addr, NULL);
    snprintf(label, sizeof label, "%s: PEEKTEXT sees the poke", what);
    check(label, errno == 0 && now == flipped);
    after[0] = '\0';
    maps_line(pid, addr, after, sizeof after);
    snprintf(label, sizeof label, "%s: tracee's maps line unchanged by it (before [%s] after [%s])",
             what, before, after);
    check(label, strcmp(before, after) == 0);
}

// Run a tracee -- exec'd fresh (gdb's shape) or just forked -- poke its text
// and vDSO while it is stopped, and collect its verdict.
static void poke_tracee(int exec_it) {
    const char *how = exec_it ? "exec'd tracee" : "forked tracee";
    int p[2];
    if (pipe(p) != 0) {
        printf("FAIL pipe: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
            _exit(101);
        if (exec_it) {
            char fdarg[16];
            snprintf(fdarg, sizeof fdarg, "%d", p[1]);
            execl("/proc/self/exe", "ptrace_poke_text", "--tracee", fdarg, (char *) NULL);
            _exit(102);
        }
        _exit(tracee_main(p[1]));
    }
    close(p[1]);
    int st;
    char label[256];
    if (exec_it) {
        // The exec's SIGTRAP stop comes first.
        waitpid(pid, &st, 0);
        snprintf(label, sizeof label, "%s: stops at its exec", how);
        check(label, WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP);
        ptrace(PTRACE_CONT, pid, NULL, NULL);
    }
    struct tracee_report r;
    ssize_t n = read(p[0], &r, sizeof r);
    close(p[0]);
    waitpid(pid, &st, 0);
    snprintf(label, sizeof label, "%s: reported and stopped", how);
    check(label, n == (ssize_t) sizeof r && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP);
    if (n != (ssize_t) sizeof r || !WIFSTOPPED(st)) {
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        return;
    }
    snprintf(label, sizeof label, "%s text", how);
    poke_flip(label, pid, (uintptr_t) r.text, r.text_byte);
    if (r.vdso != 0) {
        snprintf(label, sizeof label, "%s vDSO", how);
        poke_flip(label, pid, (uintptr_t) (r.vdso + 9), r.vdso_byte);
    } else {
        test_logf("%s: no vDSO (no AT_SYSINFO_EHDR); its poke is not checked\n", how);
    }
    // Poking an unmapped address is EIO, as a failed forced access always is.
    errno = 0;
    long rc = ptrace(PTRACE_POKEDATA, pid, (void *) (uintptr_t) 0x1000, (void *) 0L);
    snprintf(label, sizeof label, "%s: POKEDATA at an unmapped address is EIO (rc %ld errno %d)",
             how, rc, errno);
    check(label, rc == -1 && errno == EIO);
    errno = 0;
    rc = ptrace(PTRACE_PEEKDATA, pid, (void *) (uintptr_t) 0x1000, NULL);
    snprintf(label, sizeof label, "%s: PEEKDATA at an unmapped address is EIO (rc %ld errno %d)",
             how, rc, errno);
    check(label, rc == -1 && errno == EIO);

    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    waitpid(pid, &st, 0);
    int bad = WIFEXITED(st) ? WEXITSTATUS(st) : 255;
    snprintf(label, sizeof label, "%s: sees the poked text byte", how);
    check(label, !(bad & TR_TEXT_UNSEEN) && bad < 100);
    snprintf(label, sizeof label, "%s: its own store to the poked text page faults", how);
    check(label, !(bad & TR_TEXT_STORE) && bad < 100);
    snprintf(label, sizeof label, "%s: the poked text page still runs", how);
    check(label, !(bad & TR_TEXT_RUNS) && bad < 100);
    if (r.vdso != 0) {
        snprintf(label, sizeof label, "%s: sees the poked vDSO byte", how);
        check(label, !(bad & TR_VDSO_UNSEEN) && bad < 100);
        snprintf(label, sizeof label, "%s: its own store to the poked vDSO page faults", how);
        check(label, !(bad & TR_VDSO_STORE) && bad < 100);
    }
    if (bad >= 100)
        test_logf("%s: exit status %d (setup failed)\n", how, bad);
}

// ---- file mappings -------------------------------------------------------------

static char scratch[64];

static int make_scratch(void) {
    snprintf(scratch, sizeof scratch, "/tmp/ptrace_poke_text.%d", (int) getpid());
    int fd = open(scratch, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    unsigned char page[PG];
    for (int i = 0; i < 4; i++) {
        memset(page, 0x10 + i, sizeof page);
        if (write(fd, page, sizeof page) != (ssize_t) sizeof page) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static unsigned char file_byte(int fd, off_t off) {
    unsigned char b = 0;
    if (pread(fd, &b, 1, off) != 1)
        return 0;
    return b;
}

// A forced write into the second page of a 4-page read-only private mapping of
// a file: one line for the whole mapping afterwards, the file untouched, and
// an mprotect split after it names each piece at its own file offset.
static void forced_write_file_mapping(int fd) {
    unsigned char *m = mmap(NULL, 4 * PG, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    check("file mapping: mmap", m != MAP_FAILED);
    if (m == MAP_FAILED)
        return;
    uintptr_t base = (uintptr_t) m;
    struct maps_fields mf;
    check("file mapping: one line before the write",
          maps_fields_at(0, base, &mf) && mf.lo == base && mf.hi == base + 4 * PG &&
          strcmp(mf.perms, "r-xp") == 0 && mf.off == 0 && strstr(mf.path, scratch) != NULL);
    check("file mapping: forced write into page 1", mem_write(base + PG + 5, 0xab) == 0);
    check("file mapping: the process sees it", m[PG + 5] == 0xab);
    check("file mapping: the file does not", file_byte(fd, PG + 5) == 0x11);
    check("file mapping: the rest of the page is intact", m[PG + 4] == 0x11 && m[PG + 6] == 0x11);
    int one = maps_fields_at(0, base + PG, &mf);
    test_logf("file mapping after the write: %lx-%lx %s %lx %s\n", mf.lo, mf.hi, mf.perms, mf.off, mf.path);
    check("file mapping: still one r-xp line for all 4 pages, at offset 0, naming the file",
          one && mf.lo == base && mf.hi == base + 4 * PG &&
          strcmp(mf.perms, "r-xp") == 0 && mf.off == 0 && strstr(mf.path, scratch) != NULL);
    check("file mapping: a store to the written page faults", store_faults(m + PG + 5) == 1);

    // Split it round the written page: [0,1) r-x, [1,3) r--, [3,4) r-x.
    check("file mapping: mprotect of pages 1-2", mprotect(m + PG, 2 * PG, PROT_READ) == 0);
    struct maps_fields a, b, c;
    int got = maps_fields_at(0, base, &a) + maps_fields_at(0, base + PG, &b) +
              maps_fields_at(0, base + 3 * PG, &c);
    test_logf("split: %lx-%lx %s %lx | %lx-%lx %s %lx | %lx-%lx %s %lx\n",
              a.lo, a.hi, a.perms, a.off, b.lo, b.hi, b.perms, b.off, c.lo, c.hi, c.perms, c.off);
    check("file mapping: split into three lines", got == 3 &&
          a.lo == base && a.hi == base + PG && b.lo == base + PG && b.hi == base + 3 * PG &&
          c.lo == base + 3 * PG && c.hi == base + 4 * PG);
    check("file mapping: each piece's protection", got == 3 &&
          strcmp(a.perms, "r-xp") == 0 && strcmp(b.perms, "r--p") == 0 && strcmp(c.perms, "r-xp") == 0);
    check("file mapping: each piece at its own file offset", got == 3 &&
          a.off == 0 && b.off == PG && c.off == 3 * PG);
    check("file mapping: each piece names the file", got == 3 &&
          strstr(a.path, scratch) != NULL && strstr(b.path, scratch) != NULL &&
          strstr(c.path, scratch) != NULL);
    check("file mapping: the written byte survived the split", m[PG + 5] == 0xab);
    munmap(m, 4 * PG);
}

// A read-only SHARED mapping refuses a forced write; a writable one takes it.
static void forced_write_shared(int fd) {
    unsigned char *ro = mmap(NULL, PG, PROT_READ, MAP_SHARED, fd, 0);
    check("shared read-only: mmap", ro != MAP_FAILED);
    if (ro != MAP_FAILED) {
        char before[1024], after[1024];
        check("shared read-only: has a maps line", maps_line(0, (uintptr_t) ro, before, sizeof before));
        int e = mem_write((uintptr_t) ro + 7, 0xcd);
        char label[128];
        snprintf(label, sizeof label, "shared read-only: a forced write is refused with EIO (got %d)", e);
        check(label, e == EIO);
        check("shared read-only: the file is untouched", file_byte(fd, 7) == 0x10);
        check("shared read-only: the mapping is untouched", ro[7] == 0x10);
        after[0] = '\0';
        maps_line(0, (uintptr_t) ro, after, sizeof after);
        check("shared read-only: maps line unchanged", strcmp(before, after) == 0);
        check("shared read-only: a store faults", store_faults(ro + 7) == 1);
        munmap(ro, PG);
    }
    unsigned char *rw = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("shared writable: mmap", rw != MAP_FAILED);
    if (rw != MAP_FAILED) {
        char before[1024], after[1024];
        check("shared writable: has a maps line", maps_line(0, (uintptr_t) rw, before, sizeof before));
        check("shared writable: a forced write takes", mem_write((uintptr_t) rw + 9, 0xce) == 0);
        check("shared writable: the mapping sees it", rw[9] == 0xce);
        check("shared writable: so does the file", file_byte(fd, 9) == 0xce);
        after[0] = '\0';
        maps_line(0, (uintptr_t) rw, after, sizeof after);
        check("shared writable: maps line unchanged", strcmp(before, after) == 0);
        rw[9] = 0x10;
        munmap(rw, PG);
    }
}

// An ordinary copy-on-write break -- a store after a fork -- of a writable
// private file page keeps the file's line.
static void cow_break_keeps_file(int fd) {
    unsigned char *m = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    check("private writable file mapping: mmap", m != MAP_FAILED);
    if (m == MAP_FAILED)
        return;
    uintptr_t base = (uintptr_t) m;
    m[3] = 0x77;   // before the fork: private already
    char before[1024], after[1024];
    check("private writable file mapping: has a maps line",
          maps_line(0, base + PG, before, sizeof before));
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0)
        _exit(m[3] == 0x77 && m[PG + 3] == 0x11 ? 0 : 1);
    int st;
    waitpid(pid, &st, 0);
    check("private writable file mapping: the child saw the parent's pages",
          WIFEXITED(st) && WEXITSTATUS(st) == 0);
    m[PG + 3] = 0x78;   // after it: breaks copy-on-write
    after[0] = '\0';
    maps_line(0, base + PG, after, sizeof after);
    char label[2200];
    snprintf(label, sizeof label, "private writable file mapping: a store after fork keeps the "
             "maps line (before [%s] after [%s])", before, after);
    check(label, strcmp(before, after) == 0);
    struct maps_fields mf;
    check("private writable file mapping: one line, naming the file",
          maps_fields_at(0, base + PG, &mf) && mf.lo == base && mf.hi == base + 4 * PG &&
          strstr(mf.path, scratch) != NULL);
    check("private writable file mapping: the file is untouched", file_byte(fd, PG + 3) == 0x11);
    munmap(m, 4 * PG);
}

// PROT_NONE: a forced write takes and grants nothing.
static void forced_write_prot_none(void) {
    for (int touched = 0; touched <= 1; touched++) {
        const char *what = touched ? "PROT_NONE (was written)" : "PROT_NONE (never touched)";
        unsigned char *m = mmap(NULL, 3 * PG, touched ? PROT_READ | PROT_WRITE : PROT_NONE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        char label[2400];
        if (m == MAP_FAILED) {
            snprintf(label, sizeof label, "%s: mmap", what);
            check(label, 0);
            continue;
        }
        if (touched) {
            m[PG + 1] = 0x42;
            mprotect(m, 3 * PG, PROT_NONE);
        }
        uintptr_t a = (uintptr_t) m + PG + 1;
        char before[1024], after[1024];
        snprintf(label, sizeof label, "%s: has a maps line", what);
        check(label, maps_line(0, a, before, sizeof before));
        int e = mem_write(a, 0x5a);
        snprintf(label, sizeof label, "%s: a forced write takes (got %d)", what, e);
        check(label, e == 0);
        unsigned char back = 0;
        e = mem_read(a, &back);
        snprintf(label, sizeof label, "%s: a forced read returns it (got %d, %#x)", what, e, back);
        check(label, e == 0 && back == 0x5a);
        e = mem_read(a + 1, &back);
        snprintf(label, sizeof label, "%s: and the page around it (got %d, %#x)", what, e, back);
        check(label, e == 0 && back == 0);
        after[0] = '\0';
        maps_line(0, a, after, sizeof after);
        snprintf(label, sizeof label, "%s: maps line unchanged (before [%s] after [%s])",
                 what, before, after);
        check(label, strcmp(before, after) == 0);
        snprintf(label, sizeof label, "%s: a load still faults", what);
        check(label, load_faults((volatile unsigned char *) a) == 1);
        mprotect(m, 3 * PG, PROT_READ);
        snprintf(label, sizeof label, "%s: made readable, shows the byte", what);
        check(label, m[PG + 1] == 0x5a);
        munmap(m, 3 * PG);
    }
}

// mlock is the mapping's, so the copy a forced write makes is still locked, as
// a store's copy after a fork is. Seen through RLIMIT_MEMLOCK, which counts
// the pages locked, in a child that the limit binds (not root).
enum {
    LK_FORCED_LOST = 1,     // the forced write's copy lost its lock
    LK_STORE_LOST = 2,      // the copy a store made after fork lost its lock
    LK_CONTROL = 4,         // at the limit, one more page was not refused
    LK_SETUP = 10,
};

static int locked_child(int fd) {
    if (geteuid() == 0 && (setgid(65534) != 0 || setuid(65534) != 0))
        return LK_SETUP + 1;
    prctl(PR_SET_DUMPABLE, 1);  // a setuid made it undumpable: /proc/self/mem is root's
    struct rlimit rl = {3 * PG, 3 * PG};
    if (setrlimit(RLIMIT_MEMLOCK, &rl) != 0)
        return LK_SETUP + 2;
    int mfd = open("/proc/self/mem", O_RDWR);
    unsigned char *m = mmap(NULL, 2 * PG, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    unsigned char *w = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *x = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *y = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mfd < 0 || m == MAP_FAILED || w == MAP_FAILED || x == MAP_FAILED || y == MAP_FAILED)
        return LK_SETUP + 3;
    if (mlock(m, 2 * PG) != 0 || mlock(w, PG) != 0)   // three pages: the limit
        return LK_SETUP + 4;
    w[0] = 1;
    if (mlock(x, PG) == 0 || errno != ENOMEM)
        return LK_CONTROL;
    int bad = 0;
    unsigned char v = 0x77;
    if (pwrite(mfd, &v, 1, (off_t) (uintptr_t) (m + PG)) != 1 || m[PG] != 0x77)
        return LK_SETUP + 5;
    // A lost lock lets x in -- which puts the count back at the limit, so the
    // next check stands on its own.
    if (mlock(x, PG) == 0)
        bad |= LK_FORCED_LOST;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0)
        _exit(0);
    waitpid(pid, NULL, 0);
    w[1] = 2;   // breaks copy-on-write
    if (mlock(y, PG) == 0)
        bad |= LK_STORE_LOST;
    return bad;
}

static void locked_pages_stay_locked(int fd) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0)
        _exit(locked_child(fd));
    int st;
    waitpid(pid, &st, 0);
    int r = WIFEXITED(st) ? WEXITSTATUS(st) : 255;
    char label[128];
    snprintf(label, sizeof label, "mlock: the child ran (status %d)", r);
    check(label, r < LK_SETUP);
    check("mlock: at RLIMIT_MEMLOCK one more page is refused", r != LK_CONTROL);
    check("mlock: a forced write's copy of a locked page is still locked",
          r >= LK_CONTROL || !(r & LK_FORCED_LOST));
    check("mlock: a store's copy after fork is still locked",
          r >= LK_CONTROL || !(r & LK_STORE_LOST));
}

// mremap grows a file mapping from the file offset after its own last page,
// also when its front was unmapped -- so the mapping no longer starts where it
// was first mapped.
static void mremap_after_split(int fd) {
    unsigned char *m = mmap(NULL, 3 * PG, PROT_READ, MAP_PRIVATE, fd, 0);
    check("mremap: mmap", m != MAP_FAILED);
    if (m == MAP_FAILED)
        return;
    check("mremap: munmap of the first page", munmap(m, PG) == 0);
    unsigned char *g = mremap(m + PG, 2 * PG, 3 * PG, MREMAP_MAYMOVE);
    check("mremap: grows", g != MAP_FAILED);
    if (g == MAP_FAILED) {
        munmap(m + PG, 2 * PG);
        return;
    }
    char label[160];
    snprintf(label, sizeof label, "mremap: the pages are the file's second to fourth (%#x %#x %#x)",
             g[0], g[PG], g[2 * PG]);
    check(label, g[0] == 0x11 && g[PG] == 0x12 && g[2 * PG] == 0x13);
    struct maps_fields mf;
    int got = maps_fields_at(0, (uintptr_t) g, &mf);
    test_logf("mremap: %lx-%lx %s %lx %s\n", mf.lo, mf.hi, mf.perms, mf.off, mf.path);
    check("mremap: one line, at file offset 4096, naming the file",
          got && mf.lo == (uintptr_t) g && mf.hi == (uintptr_t) g + 3 * PG && mf.off == PG &&
          strstr(mf.path, scratch) != NULL);
    munmap(g, 3 * PG);
}

// A field of the smaps entry for the region holding `addr`, in kB; -1 if none.
static long smaps_kb(uintptr_t addr, const char *field) {
    FILE *f = fopen("/proc/self/smaps", "r");
    if (f == NULL)
        return -1;
    char line[512];
    int in = 0;
    long kb = -1;
    size_t n = strlen(field);
    while (fgets(line, sizeof line, f) != NULL) {
        unsigned long lo, hi;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3 && strchr(line, ':') > strchr(line, ' ')) {
            in = addr >= lo && addr < hi;
            continue;
        }
        if (in && strncmp(line, field, n) == 0) {
            kb = strtol(line + n, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kb;
}

static long minor_faults(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_minflt;
}

// The page a forced write copies is the process's own after that: gdb takes
// its breakpoints out and puts them back at every stop, and Linux writes them
// in place (0 minor faults for 50 pokes after the first).
static void repeat_pokes_in_place(void) {
    volatile unsigned char *v = (volatile unsigned char *) (uintptr_t) &victim_fn;
    unsigned char orig = *v;
    check("repeat pokes: the first one", mem_write((uintptr_t) v, (unsigned char) (orig ^ 0xff)) == 0);
    long before = minor_faults();
    int ok = 1;
    for (int i = 0; i < 50; i++)
        ok &= mem_write((uintptr_t) v, (unsigned char) (i & 1 ? orig : orig ^ 0xff)) == 0;
    long faults = minor_faults() - before;
    check("repeat pokes: 50 more take", ok && *v == orig);
    char label[128];
    snprintf(label, sizeof label, "repeat pokes: 50 more cost fewer than 10 minor faults (%ld)", faults);
    check(label, faults < 10);
    check("repeat pokes: the page still faults a store", store_faults(v) == 1);
}

// smaps counts a forced write's copy the way Linux does: anonymous, private
// and dirty, inside its file's region -- the only page of it resident here.
static void smaps_counts_the_copy(int fd) {
    unsigned char *t = mmap(NULL, 3 * PG, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    if (t == MAP_FAILED) {
        check("smaps: mmap", 0);
        return;
    }
    // Nothing reads the mapping: the copy is the one resident page.
    check("smaps: forced write into page 1 of 3", mem_write((uintptr_t) t + PG + 3, 0x66) == 0);
    long rss = smaps_kb((uintptr_t) t, "Rss:");
    long anon = smaps_kb((uintptr_t) t, "Anonymous:");
    long dirty = smaps_kb((uintptr_t) t, "Private_Dirty:");
    char label[160];
    snprintf(label, sizeof label, "smaps: the region's Rss, Anonymous and Private_Dirty are the "
             "copy's 4 kB (%ld, %ld, %ld)", rss, anon, dirty);
    check(label, rss == 4 && anon == 4 && dirty == 4);
    munmap(t, 3 * PG);
}

// A page of a file mapping past the end of its file: a forced read or write of
// it is EIO, and a store that has to copy it (after a fork) is SIGBUS -- never
// the end of the emulator, which is what AOK's plain memcpy of it was.
static void past_end_of_file(void) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/ptrace_poke_text.eof.%d", (int) getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    unsigned char bytes[100];
    memset(bytes, 'A', sizeof bytes);
    if (fd < 0 || write(fd, bytes, sizeof bytes) != (ssize_t) sizeof bytes) {
        check("past EOF: a 100-byte file", 0);
        if (fd >= 0)
            close(fd);
        unlink(path);
        return;
    }
    unlink(path);
    // Page 16 is past the end on a 4K and a 16K host page alike.
    unsigned char *ro = mmap(NULL, 20 * PG, PROT_READ, MAP_PRIVATE, fd, 0);
    unsigned char *rw = mmap(NULL, 20 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);
    if (ro == MAP_FAILED || rw == MAP_FAILED) {
        check("past EOF: mmap", 0);
        return;
    }
    uintptr_t far = 16 * PG;
    unsigned char b = 0;
    char label[128];
    int e = mem_read((uintptr_t) ro + far, &b);
    snprintf(label, sizeof label, "past EOF: a forced read is EIO (got %d)", e);
    check(label, e == EIO);
    e = mem_write((uintptr_t) ro + far, 1);
    snprintf(label, sizeof label, "past EOF: a forced write to a read-only page is EIO (got %d)", e);
    check(label, e == EIO);
    e = mem_write((uintptr_t) rw + far, 1);
    snprintf(label, sizeof label, "past EOF: a forced write to a writable page is EIO (got %d)", e);
    check(label, e == EIO);
    mprotect(ro + far, PG, PROT_NONE);
    e = mem_read((uintptr_t) ro + far, &b);
    snprintf(label, sizeof label, "past EOF: a forced read of it made PROT_NONE is EIO (got %d)", e);
    check(label, e == EIO);
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        rw[far] = 1;
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    snprintf(label, sizeof label, "past EOF: a store after fork is SIGBUS (status %#x)", st);
    check(label, WIFSIGNALED(st) && WTERMSIG(st) == SIGBUS);
    munmap(ro, 20 * PG);
    munmap(rw, 20 * PG);
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--tracee") == 0)
        return tracee_main(atoi(argv[2]));
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    setvbuf(stdout, NULL, _IONBF, 0);

    mem_fd = open("/proc/self/mem", O_RDWR);
    check("/proc/self/mem opens read-write", mem_fd >= 0);
    if (mem_fd < 0)
        return finish_suite("ptrace_poke_text");

    // Before any fork, so no page of this process is copy-on-write yet.
    forced_write_readonly("text page", (uintptr_t) &victim_fn, "r-xp");
    check("text page: still runs", still_runs(3) == 16);
    uintptr_t vdso = (uintptr_t) getauxval(AT_SYSINFO_EHDR);
    if (vdso != 0) {
        forced_write_readonly("vDSO", vdso + 9, "r-xp");
        struct timespec ts;
        check("vDSO: clock_gettime still works", clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    } else {
        test_logf("no vDSO (no AT_SYSINFO_EHDR); its forced write is not checked\n");
    }
    forced_access_vvar();
    repeat_pokes_in_place();
    past_end_of_file();
    int e = mem_write(0x1000, 0);
    char label[128];
    snprintf(label, sizeof label, "/proc/self/mem at an unmapped address is EIO (got %d)", e);
    check(label, e == EIO);

    int fd = make_scratch();
    check("scratch file", fd >= 0);
    if (fd >= 0) {
        forced_write_file_mapping(fd);
        forced_write_shared(fd);
        forced_write_prot_none();
        locked_pages_stay_locked(fd);
        mremap_after_split(fd);
        smaps_counts_the_copy(fd);
        cow_break_keeps_file(fd);
    }

    poke_tracee(1);
    poke_tracee(0);

    // After those forks every private page here is copy-on-write: again.
    forced_write_readonly("text page after a fork", (uintptr_t) &victim_fn, "r-xp");
    if (vdso != 0)
        forced_write_readonly("vDSO after a fork", vdso + 9, "r-xp");
    check("text page after a fork: still runs", still_runs(4) == 21);

    if (fd >= 0) {
        close(fd);
        unlink(scratch);
    }
    close(mem_fd);
    return finish_suite("ptrace_poke_text");
}
