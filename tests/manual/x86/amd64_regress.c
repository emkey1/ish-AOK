// amd64 regressions: a store across a page, COW atomics after fork, ELF loader
// edge cases, an fcntl lock racing close, PUSH/POP order, CMOV's zero
// extension, and cc1 compiling under stress.
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

static volatile unsigned char exec_bss_guard[1 << 20];

struct lock_race_state {
    int base_fd;
    volatile int shared_fd;
    volatile int stop;
    volatile unsigned long setlk_ok;
    volatile unsigned long setlk_ebadf;
    volatile unsigned long setlk_other;
    volatile unsigned long reopen_ok;
};

static int run_wait_ok(pid_t pid, int *status_out) {
    int status = 0;
    int rc = waitpid(pid, &status, 0);
    if (rc != pid)
        return -1;
    if (status_out != NULL)
        *status_out = status;
    return 0;
}

static int helper_exec_child(void) {
    size_t i;
    for (i = 0; i < sizeof(exec_bss_guard); i++) {
        if (exec_bss_guard[i] != 0)
            return 91;
    }
    exec_bss_guard[0] = 1;
    exec_bss_guard[sizeof(exec_bss_guard) - 1] = 2;
    return 0;
}

static void fill_pattern(int fd, size_t len) {
    unsigned char *buf = malloc(len);
    size_t i;
    ssize_t written;

    if (buf == NULL) {
        perror("malloc");
        exit(1);
    }
    for (i = 0; i < len; i++)
        buf[i] = (unsigned char) ((i * 37u) ^ (i >> 3));

    written = pwrite(fd, buf, len, 0);
    if (written != (ssize_t) len) {
        perror("pwrite");
        exit(1);
    }
    free(buf);
}

static void test_cross_page_private_store(void) {
    static const uint64_t value = 0x1122334455667788ULL;
    char template[] = "/tmp/ish-aok-amd64-cross-page-XXXXXX";
    const unsigned char expected[8] = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    long page_size = sysconf(_SC_PAGESIZE);
    int fd;
    int off;
    int iter;

    if (page_size <= 0) {
        perror("sysconf");
        exit(1);
    }

    fd = mkstemp(template);
    if (fd < 0) {
        perror("mkstemp");
        exit(1);
    }
    unlink(template);

    if (ftruncate(fd, page_size * 2) != 0) {
        perror("ftruncate");
        exit(1);
    }
    fill_pattern(fd, (size_t) page_size * 2);

    for (iter = 0; iter < 32; iter++) {
        for (off = 1; off <= 7; off++) {
            unsigned char *map = mmap(NULL, (size_t) page_size * 2, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE, fd, 0);
            unsigned char file_bytes[8];
            unsigned char original_bytes[8];
            volatile uint64_t *slot;
            int failed;

            if (map == MAP_FAILED) {
                perror("mmap");
                exit(1);
            }

            memcpy(original_bytes, map + page_size - off, sizeof(original_bytes));
            slot = (volatile uint64_t *) (map + page_size - off);
            *slot = value;

            failed = memcmp((const void *) (map + page_size - off), expected, sizeof(expected)) != 0;
            if (failed) {
                test_log_if(1, "cross page store mismatch iter=%d off=%d\n", iter, off);
                failf("amd64 cross page store map", (uint64_t) map[page_size - off + 0],
                      (uint64_t) map[page_size - off + 1], (uint64_t) map[page_size - off + 2],
                      expected[0], expected[1], expected[2]);
            }

            if (pread(fd, file_bytes, sizeof(file_bytes), page_size - off) != (ssize_t) sizeof(file_bytes)) {
                perror("pread");
                exit(1);
            }
            failed = memcmp(file_bytes, original_bytes, sizeof(file_bytes)) != 0;
            test_log_if(failed, "cross page file backing changed iter=%d off=%d\n", iter, off);
            if (failed)
                failf("amd64 cross page backing", file_bytes[0], file_bytes[1], file_bytes[2],
                      original_bytes[0], original_bytes[1], original_bytes[2]);

            if (munmap(map, (size_t) page_size * 2) != 0) {
                perror("munmap");
                exit(1);
            }
        }
    }

    close(fd);
}

static void test_exec_loader_zero(char **argv) {
    int iter;

    for (iter = 0; iter < 64; iter++) {
        pid_t pid = fork();
        int status = 0;

        if (pid < 0) {
            perror("fork");
            exit(1);
        }
        if (pid == 0) {
            char *child_argv[] = { argv[0], "--exec-child", NULL };
            execv(argv[0], child_argv);
            _exit(127);
        }

        if (run_wait_ok(pid, &status) != 0) {
            perror("waitpid");
            exit(1);
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            test_log_if(1, "exec loader zero failed iter=%d status=%#x\n", iter, status);
            failf("amd64 exec loader zero", (uint64_t) WIFEXITED(status),
                  (uint64_t) WEXITSTATUS(status), (uint64_t) status, 1, 0, 0);
            return;
        }
    }
}

static void put_le(unsigned char *p, uint64_t v, int n) {
    for (int i = 0; i < n; i++)
        p[i] = (unsigned char) (v >> (8 * i));
}

#if defined(__x86_64__) || defined(__i386__)
// Build a minimal static ELF whose single writable PT_LOAD ends partway through
// the final host page of its file mapping, with a BSS tail past p_filesz. On a
// host whose page size exceeds the guest's (iOS: 16K host vs 4K guest), the ELF
// loader used to map that final, EOF-straddling page directly from the file and
// then zero-fill the BSS tail into it; the copy-on-write pagein cluster spilled
// past the backing file's EOF and APFS killed the guest with SIGBUS
// ("cluster_pagein past EOF"). The entry point sits *inside* that straddling
// region, so a clean exit also proves the loader reproduced those bytes (now
// copied into anonymous memory) intact.
static int build_straddle_elf_fd(int fd) {
    const uint64_t filesz = 0x12500;          // file content ends mid last host page
    const uint64_t memsz = filesz + 0x8000;   // BSS past p_filesz
    const uint32_t entry_off = 0x124f0;       // inside the straddling/copied region
#if defined(__x86_64__)
    const uint64_t base = 0x400000;
    const unsigned char code[] = { 0x31,0xFF, 0xB8,60,0,0,0, 0x0F,0x05 }; // xor edi,edi; mov eax,60; syscall
#else
    const uint64_t base = 0x08048000;
    const unsigned char code[] = { 0xB8,1,0,0,0, 0x31,0xDB, 0xCD,0x80 };  // mov eax,1; xor ebx,ebx; int 0x80
#endif
    unsigned char *f = calloc(1, filesz);
    if (f == NULL)
        return -1;
    memcpy(f, "\x7f""ELF", 4);
    f[5] = 1; f[6] = 1;                        // little-endian, EI_VERSION
    put_le(f + 16, 2, 2);                      // e_type = ET_EXEC
    put_le(f + 20, 1, 4);                      // e_version
#if defined(__x86_64__)
    f[4] = 2;                                  // ELFCLASS64
    put_le(f + 18, 62, 2);                     // EM_X86_64
    put_le(f + 24, base + entry_off, 8);       // e_entry
    put_le(f + 32, 64, 8);                     // e_phoff
    put_le(f + 52, 64, 2);                     // e_ehsize
    put_le(f + 54, 56, 2);                     // e_phentsize
    put_le(f + 56, 1, 2);                      // e_phnum
    unsigned char *ph = f + 64;
    put_le(ph + 0, 1, 4);                      // PT_LOAD
    put_le(ph + 4, 7, 4);                      // PF_R|W|X
    put_le(ph + 8, 0, 8);                      // p_offset
    put_le(ph + 16, base, 8);                  // p_vaddr
    put_le(ph + 24, base, 8);                  // p_paddr
    put_le(ph + 32, filesz, 8);                // p_filesz
    put_le(ph + 40, memsz, 8);                 // p_memsz
    put_le(ph + 48, 0x1000, 8);                // p_align
#else
    f[4] = 1;                                  // ELFCLASS32
    put_le(f + 18, 3, 2);                      // EM_386
    put_le(f + 24, base + entry_off, 4);       // e_entry
    put_le(f + 28, 52, 4);                     // e_phoff
    put_le(f + 40, 52, 2);                     // e_ehsize
    put_le(f + 42, 32, 2);                     // e_phentsize
    put_le(f + 44, 1, 2);                      // e_phnum
    unsigned char *ph = f + 52;
    put_le(ph + 0, 1, 4);                      // PT_LOAD
    put_le(ph + 4, 0, 4);                      // p_offset
    put_le(ph + 8, base, 4);                   // p_vaddr
    put_le(ph + 12, base, 4);                  // p_paddr
    put_le(ph + 16, filesz, 4);                // p_filesz
    put_le(ph + 20, memsz, 4);                 // p_memsz
    put_le(ph + 24, 7, 4);                     // PF_R|W|X
    put_le(ph + 28, 0x1000, 4);                // p_align
#endif
    memcpy(f + entry_off, code, sizeof(code));

    ssize_t wr = write(fd, f, filesz);
    free(f);
    if (wr != (ssize_t) filesz)
        return -1;
    if (fchmod(fd, 0755) != 0)
        return -1;
    return 0;
}

static void test_exec_loader_straddle(void) {
    char path[] = "/tmp/ish-aok-straddle-XXXXXX";
    int tfd = mkstemp(path);
    if (tfd < 0) {
        perror("mkstemp");
        exit(1);
    }
    if (build_straddle_elf_fd(tfd) != 0) {
        test_log_if(1, "could not build straddle elf\n");
        failf("amd64 exec loader straddle build", 0, 0, 0, 1, 0, 0);
        close(tfd);
        unlink(path);
        return;
    }
    close(tfd);

    for (int iter = 0; iter < 8; iter++) {
        pid_t pid = fork();
        int status = 0;

        if (pid < 0) {
            perror("fork");
            exit(1);
        }
        if (pid == 0) {
            char *child_argv[] = { path, NULL };
            execv(path, child_argv);
            _exit(127);
        }
        if (run_wait_ok(pid, &status) != 0) {
            perror("waitpid");
            exit(1);
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            test_log_if(1, "exec loader straddle failed iter=%d status=%#x\n", iter, status);
            failf("amd64 exec loader straddle", (uint64_t) WIFEXITED(status),
                  (uint64_t) (WIFSIGNALED(status) ? WTERMSIG(status) : 0),
                  (uint64_t) status, 1, 0, 0);
            unlink(path);
            return;
        }
    }
    unlink(path);
    test_logf("exec loader straddle: ok\n");
}
#else
static void test_exec_loader_straddle(void) {
    test_logf("exec loader straddle: skipped (non-x86)\n");
}
#endif

static void *lock_race_fcntl_thread(void *arg) {
    struct lock_race_state *state = arg;
    struct flock fl;
    unsigned long iter;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_CUR;
    fl.l_start = 0;
    fl.l_len = 1;

    for (iter = 0; iter < 200000; iter++) {
        int fd = __atomic_load_n(&state->shared_fd, __ATOMIC_ACQUIRE);
        int rc;
        int saved_errno;

        if (fd < 0) {
            sched_yield();
            continue;
        }
        rc = fcntl(fd, F_SETLK, &fl);
        saved_errno = errno;
        if (rc == 0)
            __atomic_add_fetch(&state->setlk_ok, 1, __ATOMIC_RELAXED);
        else if (saved_errno == EBADF)
            __atomic_add_fetch(&state->setlk_ebadf, 1, __ATOMIC_RELAXED);
        else
            __atomic_add_fetch(&state->setlk_other, 1, __ATOMIC_RELAXED);
        if ((iter & 255u) == 0)
            sched_yield();
    }

    __atomic_store_n(&state->stop, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *lock_race_reopen_thread(void *arg) {
    struct lock_race_state *state = arg;

    while (!__atomic_load_n(&state->stop, __ATOMIC_ACQUIRE)) {
        int old_fd = __atomic_exchange_n(&state->shared_fd, -1, __ATOMIC_ACQ_REL);
        int new_fd;

        if (old_fd >= 0)
            close(old_fd);

        new_fd = dup(state->base_fd);
        if (new_fd >= 0) {
            __atomic_add_fetch(&state->reopen_ok, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&state->shared_fd, new_fd, __ATOMIC_RELEASE);
        }
        sched_yield();
    }
    return NULL;
}

static void test_fcntl_lock_close_race(void) {
    char template[] = "/tmp/ish-aok-amd64-lock-race-XXXXXX";
    struct lock_race_state state;
    pthread_t fcntl_thread;
    pthread_t reopen_thread;
    int fd;
    int failed;

    memset(&state, 0, sizeof(state));
    fd = mkstemp(template);
    if (fd < 0) {
        perror("mkstemp");
        exit(1);
    }
    unlink(template);
    if (ftruncate(fd, 4096) != 0) {
        perror("ftruncate");
        exit(1);
    }

    state.base_fd = fd;
    state.shared_fd = dup(fd);
    if (state.shared_fd < 0) {
        perror("dup");
        exit(1);
    }

    if (pthread_create(&fcntl_thread, NULL, lock_race_fcntl_thread, &state) != 0) {
        perror("pthread_create");
        exit(1);
    }
    if (pthread_create(&reopen_thread, NULL, lock_race_reopen_thread, &state) != 0) {
        perror("pthread_create");
        exit(1);
    }

    pthread_join(fcntl_thread, NULL);
    pthread_join(reopen_thread, NULL);

    if (state.shared_fd >= 0)
        close((int) state.shared_fd);
    close(fd);

    failed = state.setlk_ok == 0 || state.reopen_ok == 0 || state.setlk_other != 0;
    test_log_if(failed,
                "fcntl lock race: ok=%lu ebadf=%lu other=%lu reopen=%lu\n",
                state.setlk_ok, state.setlk_ebadf, state.setlk_other, state.reopen_ok);
    if (failed)
        failf("amd64 fcntl lock race", state.setlk_ok, state.setlk_ebadf,
              state.setlk_other, 1, 0, 0);
}

static int write_text_file(const char *path, const char *text) {
    size_t len = strlen(text);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    ssize_t written;

    if (fd < 0)
        return -1;
    written = write(fd, text, len);
    close(fd);
    return written == (ssize_t) len ? 0 : -1;
}

static int run_command(char *const argv[]) {
    pid_t pid = fork();
    int status = 0;

    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (run_wait_ok(pid, &status) != 0)
        return -1;
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static void test_cc1_varargs_compile_stress(void) {
    static const char source_text[] =
        "#include <stdarg.h>\n"
        "#include <stdint.h>\n"
        "struct node { long a; long b; long c; long d; };\n"
        "static volatile long sink;\n"
        "__attribute__((noinline))\n"
        "static long consume(int tag, int count, ...) {\n"
        "    va_list ap, copy;\n"
        "    long acc = tag;\n"
        "    int i;\n"
        "    va_start(ap, count);\n"
        "    va_copy(copy, ap);\n"
        "    for (i = 0; i < count; i++) {\n"
        "        long v0 = va_arg(ap, long);\n"
        "        long v1 = va_arg(copy, long);\n"
        "        acc += (v0 ^ (v1 + i)) - (acc >> (i & 7));\n"
        "        if ((i & 3) == 0)\n"
        "            acc ^= (v0 << (i & 15));\n"
        "    }\n"
        "    va_end(copy);\n"
        "    va_end(ap);\n"
        "    return acc;\n"
        "}\n"
        "__attribute__((noinline))\n"
        "static long drive(const struct node *nodes, int n) {\n"
        "    long acc = 0;\n"
        "    int i;\n"
        "    for (i = 0; i < n; i++) {\n"
        "        acc += consume(i, 6,\n"
        "                nodes[i].a, nodes[i].b, nodes[i].c,\n"
        "                nodes[i].d, nodes[i].a + nodes[i].c,\n"
        "                nodes[i].b + nodes[i].d);\n"
        "        if ((acc & 7) == 3)\n"
        "            acc ^= nodes[i].a + nodes[i].d;\n"
        "    }\n"
        "    return acc;\n"
        "}\n"
        "int main(void) {\n"
        "    struct node nodes[64];\n"
        "    int i;\n"
        "    for (i = 0; i < 64; i++) {\n"
        "        nodes[i].a = i + 1;\n"
        "        nodes[i].b = i * 3 + 2;\n"
        "        nodes[i].c = i * 5 + 7;\n"
        "        nodes[i].d = i * 7 + 11;\n"
        "    }\n"
        "    sink = drive(nodes, 64);\n"
        "    return sink == 0x7fffffff ? 1 : 0;\n"
        "}\n";
    char dir_template[] = "/tmp/ish-aok-amd64-cc1-XXXXXX";
    char src_path[256];
    char out_path[256];
    char *compiler = NULL;
    char *cmd_argv[10];
    int rc;
    int iter;

    if (access("/usr/bin/gcc", X_OK) == 0)
        compiler = "/usr/bin/gcc";
    else if (access("/bin/gcc", X_OK) == 0)
        compiler = "/bin/gcc";
    else if (access("/usr/bin/cc", X_OK) == 0)
        compiler = "/usr/bin/cc";
    else if (access("/bin/cc", X_OK) == 0)
        compiler = "/bin/cc";

    if (compiler == NULL) {
        test_logf("skip cc1 stress: no gcc/cc found\n");
        return;
    }

    if (mkdtemp(dir_template) == NULL) {
        perror("mkdtemp");
        exit(1);
    }

    snprintf(src_path, sizeof(src_path), "%s/cc1-varargs.c", dir_template);
    snprintf(out_path, sizeof(out_path), "%s/cc1-varargs.s", dir_template);
    if (write_text_file(src_path, source_text) != 0) {
        perror("write");
        exit(1);
    }

    for (iter = 0; iter < 8; iter++) {
        int argc = 0;
        cmd_argv[argc++] = compiler;
        cmd_argv[argc++] = "-O2";
        cmd_argv[argc++] = "-fno-inline";
        cmd_argv[argc++] = "-fno-tree-vectorize";
        cmd_argv[argc++] = "-fno-omit-frame-pointer";
        cmd_argv[argc++] = "-S";
        cmd_argv[argc++] = src_path;
        cmd_argv[argc++] = "-o";
        cmd_argv[argc++] = out_path;
        cmd_argv[argc] = NULL;

        rc = run_command(cmd_argv);
        test_log_if(rc != 0, "cc1 stress iteration=%d rc=%d compiler=%s\n", iter, rc, compiler);
        if (rc != 0) {
            failf("amd64 cc1 stress", (uint64_t) rc, (uint64_t) iter, 0, 0, 0, 0);
            break;
        }
    }
}

// Native JIT push/pop gadgets: push r8-r15 (REX.B-encoded) and pop them back in
// reverse, verifying strict LIFO order and 64-bit value integrity. The compiler
// also saves/restores its own callee-saved regs around this block, adding more
// push/pop. (gen.c emits gadget_amd64_push_reg/pop_reg for 0x50-0x5f.)
static void test_push_pop_lifo(void) {
    uint64_t out[8];
    uint64_t *p = out;
    __asm__ volatile(
        "movq $0x000000000000A000, %%r8\n\t"
        "movq $0x000000000000B000, %%r9\n\t"
        "movq $0x000000000000C000, %%r10\n\t"
        "movq $0x000000000000D000, %%r11\n\t"
        "movq $0x000000000000E000, %%r12\n\t"
        "movq $0x000000000000F000, %%r13\n\t"
        "movq $0x000000000001A000, %%r14\n\t"
        "movq $0x000000000001B000, %%r15\n\t"
        "pushq %%r8\n\t pushq %%r9\n\t pushq %%r10\n\t pushq %%r11\n\t"
        "pushq %%r12\n\t pushq %%r13\n\t pushq %%r14\n\t pushq %%r15\n\t"
        "popq %%rax\n\t movq %%rax, 0(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 8(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 16(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 24(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 32(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 40(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 48(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 56(%0)\n\t"
        : : "r"(p)
        : "rax", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "memory");
    static const uint64_t want[8] = {
        0x1B000, 0x1A000, 0xF000, 0xE000, 0xD000, 0xC000, 0xB000, 0xA000
    };
    for (int i = 0; i < 8; i++) {
        if (out[i] != want[i]) {
            failf("push_pop_lifo", (uint64_t) i, out[i], want[i], 0, 0, 0);
            return;
        }
    }
    test_logf("push_pop_lifo: ok\n");
}

// CMOVcc writes its destination EVEN WHEN THE CONDITION IS FALSE, and a
// 32-bit write zero-extends -- so a not-taken 32-bit cmov clears the upper half
// of the register. Verified on real x86-64 hardware: destination
// 0xAAAAAAAAFFFFFFFF with a false condition comes back 0x00000000FFFFFFFF.
//
// AOK got this wrong in BOTH of its CMOVcc implementations (emu/amd64_interp.c
// has two copies), writing only when the condition held and leaving whatever
// was in the top 32 bits. gcc emits cmov constantly, so the stale half was
// reachable from ordinary compiled code.
static void test_cmov_false_zero_extends(void) {
    uint64_t dst = 0xAAAAAAAAFFFFFFFFull;
    uint32_t src = 0x55667788u;
    __asm__ volatile("cmpl $1, %2\n\t"      /* 0 == 1 is false */
                     "cmovel %1, %k0"
                     : "+r"(dst) : "r"(src), "r"(0) : "cc");
    if (dst != 0x00000000FFFFFFFFull) {
        failf("cmov_false_zero_extends", 0, dst, 0x00000000FFFFFFFFull, 0, 0, 0);
        return;
    }
    // And the taken case must move AND zero-extend.
    uint64_t dst2 = 0xAAAAAAAA11111111ull;
    __asm__ volatile("cmpl $0, %2\n\t"      /* 0 == 0 is true */
                     "cmovel %1, %k0"
                     : "+r"(dst2) : "r"(src), "r"(0) : "cc");
    if (dst2 != 0x0000000055667788ull) {
        failf("cmov_true_zero_extends", 0, dst2, 0x0000000055667788ull, 0, 0, 0);
        return;
    }
    // 64-bit operand size does NOT zero-extend anything; a false condition
    // must leave the register completely untouched.
    uint64_t dst3 = 0xAAAAAAAA11111111ull;
    uint64_t src3 = 0x1122334455667788ull;
    __asm__ volatile("cmpl $1, %2\n\t"
                     "cmoveq %1, %0"
                     : "+r"(dst3) : "r"(src3), "r"(0) : "cc");
    if (dst3 != 0xAAAAAAAA11111111ull) {
        failf("cmov_false_64bit_untouched", 0, dst3, 0xAAAAAAAA11111111ull, 0, 0, 0);
        return;
    }
    test_logf("cmov_false_zero_extends: ok\n");
}

// Locked RMW in a fork child, on pages the child has not written yet. After
// fork every private page is copy-on-write, and the JIT's atomic gadgets
// (lock imm, xchg, cmpxchg) take their host pointer from the nofault
// translation, which refuses a not-yet-writable page: the C helper returns
// INT_PF and the gadget must deliver it so the kernel breaks COW and the
// instruction retries. Until 2026-09-06 three gadget shells exited that path
// with INT_NONE instead, so the fault vanished and a CAS loop in a fresh child
// spun forever (amd64_regress itself wedged for half an hour under the JIT).
// Reading the page first is deliberate: it puts a read entry in the TLB, the
// shape the failure needs.
static uint32_t fork_cow_v = 5, fork_cow_w = 5, fork_cow_x = 5;
static void test_fork_cow_atomics(void) {
    pid_t pid = fork();
    if (pid < 0) { failf("fork cow atomics: fork", 0, 0, 0, 0, 0, 0); return; }
    if (pid == 0) {
        volatile uint32_t sink = fork_cow_v + fork_cow_w + fork_cow_x;
        (void) sink;
        uint32_t exp = 5;
        int ok = __atomic_compare_exchange_n(&fork_cow_v, &exp, 11u, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&fork_cow_w, 6, __ATOMIC_SEQ_CST);
        uint32_t old = __atomic_exchange_n(&fork_cow_x, 11u, __ATOMIC_SEQ_CST);
        long i;
        for (i = 0; i < 1000000; i++) {
            exp = __atomic_load_n(&fork_cow_v, __ATOMIC_RELAXED);
            if (__atomic_compare_exchange_n(&fork_cow_v, &exp, exp + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                break;
        }
        _exit((ok == 1 && fork_cow_v == 12 && fork_cow_w == 11 && old == 5 && fork_cow_x == 11 && i == 0) ? 0 : 1);
    }
    int st = -1;
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
        failf("fork cow atomics: child", (uint64_t) st, 0, 0, 0, 0, 0);
    if (fork_cow_v != 5 || fork_cow_w != 5 || fork_cow_x != 5)
        failf("fork cow atomics: parent saw child writes", fork_cow_v, fork_cow_w, fork_cow_x, 5, 5, 5);
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--exec-child") == 0)
        return helper_exec_child();

    test_init(argc, argv);
    test_cross_page_private_store();
    test_fork_cow_atomics();
    test_exec_loader_zero(argv);
    test_exec_loader_straddle();
    test_fcntl_lock_close_race();
    test_push_pop_lifo();
    test_cmov_false_zero_extends();
    test_cc1_varargs_compile_stress();
    return finish_suite("amd64_regress");
}
