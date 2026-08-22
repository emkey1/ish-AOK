// Fun little utility that single-steps a program using ptrace and
// simultaneously runs the program in ish, and asserts that everything's
// working the same.
// Many apologies for the messy code.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#undef PAGE_SIZE // defined in sys/user.h, but we want the version from emu/memory.h
#include <sys/personality.h>
#include <sys/socket.h>

#include "debug.h"
#include "kernel/abi.h"
#include "kernel/calls.h"
#include "fs/path.h"
#include "fs/fd.h"
#include "emu/interrupt.h"
#include "emu/cpuid.h"

#include "kernel/elf.h"
#include "tools/transplant.h"
#include "tools/ptutil.h"
#include "undefined-flags.h"
#include "kernel/vdso.h"

#include "xX_main_Xx.h"

// ptrace utility functions

// returns 1 for a signal stop
// Why this exists rather than printk.
//
// A divergence report is the only output this tool produces that anyone wants,
// and it went to printk -- which writes to file descriptor 555 (kernel/log.c),
// the convention the emulator uses for its own log. Nobody redirects 555 when
// running this by hand, so writev failed with EBADF and the report vanished.
// Then `debugger` fired an int3, and the whole session was a shell reporting
// "Trace/breakpoint trap" with not one line explaining what had differed.
// Divergences go to stderr.
static void reportf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fflush(stderr);
}

// int3 with no debugger attached is not a breakpoint, it is a crash. The trap
// is what a person single-stepping this in gdb wants and it is exactly wrong
// for everyone else, so it is opt-in: PTRACEOMATIC_TRAP=1 to keep the old
// behaviour, and by default the report on stderr is the answer.
static bool trap_wanted(void) {
    static int want = -1;
    if (want < 0) {
        const char *v = getenv("PTRACEOMATIC_TRAP");
        want = (v != NULL && *v != '\0' && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return want == 1;
}

// A syscall the interception switch does not know about still gets its return
// value synced, so a missing case is invisible until the guest reads the buffer
// the syscall was supposed to fill -- at which point the report blames whatever
// innocent instruction did the reading. This trace names the syscalls that went
// through with no memory sync, which is where to look first.
static bool trace_syscalls(void) {
    static int want = -1;
    if (want < 0) {
        const char *v = getenv("PTRACEOMATIC_TRACE_SYSCALLS");
        want = (v != NULL && *v != '\0' && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return want == 1;
}

static inline int step(int pid) {
    trycall(ptrace(PTRACE_SINGLESTEP, pid, NULL, 0), "ptrace step");
    int status;
    trycall(waitpid(pid, &status, 0), "wait step");
    if (WIFSTOPPED(status) && WSTOPSIG(status) != SIGTRAP) {
        int signal = WSTOPSIG(status);
        reportf("ptraceomatic: tracee received signal %d (%s)\n", signal, strsignal(signal));
        // a signal arrived, we now have to actually deliver it
        trycall(ptrace(PTRACE_SINGLESTEP, pid, NULL, signal), "ptrace step");
        trycall(waitpid(pid, &status, 0), "wait step");
        return 1;
    }
    return 0;
}

// ptrace requests must come from the thread that attached, and a tracee that
// is running rather than stopped rejects them too. Both show up as a bare
// ESRCH, so report the state that actually explains it.
static void diagnose_ptrace_esrch(int pid, const char *what) {
    fprintf(stderr, "ptraceomatic: %s failed: %s\n", what, strerror(errno));
    fprintf(stderr, "  caller pid=%d tid=%ld\n", (int) getpid(), (long) syscall(SYS_gettid));
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "  tracee %d: no /proc entry, it is gone\n", pid);
        exit(1);
    }
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL)
        if (strncmp(line, "State:", 6) == 0 || strncmp(line, "TracerPid:", 10) == 0 ||
            strncmp(line, "Pid:", 4) == 0)
            fprintf(stderr, "  tracee %s", line);
    fclose(f);
    exit(1);
}

static inline void getregs(int pid, struct user_regs_struct *regs) {
    if (ptrace(PTRACE_GETREGS, pid, NULL, regs) < 0)
        diagnose_ptrace_esrch(pid, "PTRACE_GETREGS");
}

static inline void setregs(int pid, struct user_regs_struct *regs) {
    trycall(ptrace(PTRACE_SETREGS, pid, NULL, regs), "ptrace setregs");
}

static int compare_cpus(struct cpu_state *cpu, struct tlb *tlb, int pid, int undefined_flags) {
    struct user_regs_struct regs;
    struct user_fpregs_struct fpregs;
    getregs(pid, &regs);
    trycall(ptrace(PTRACE_GETFPREGS, pid, NULL, &fpregs), "ptrace getregs compare");
    collapse_flags(cpu);
#define CHECK(real, fake, fmt, ...) do { \
    if ((real) != (fake)) { \
        reportf(fmt ": real 0x%llx, fake 0x%llx\n", ##__VA_ARGS__, (unsigned long long) (real), (unsigned long long) (fake)); \
        if (trap_wanted()) debugger; \
        return -1; \
    } \
} while (0)
#define CHECK_REG(pt, cp) CHECK(regs.pt, cpu->cp, #cp)
#define CHECK_REG64(pt, idx) CHECK((qword_t) regs.pt, cpu->amd64_regs[idx], #pt)
    if (current->abi == GUEST_ABI_AMD64) {
        CHECK_REG64(rax, amd64_rax);
        CHECK_REG64(rbx, amd64_rbx);
        CHECK_REG64(rcx, amd64_rcx);
        CHECK_REG64(rdx, amd64_rdx);
        CHECK_REG64(rsi, amd64_rsi);
        CHECK_REG64(rdi, amd64_rdi);
        CHECK_REG64(rsp, amd64_rsp);
        CHECK_REG64(rbp, amd64_rbp);
        CHECK_REG64(r8, amd64_r8);
        CHECK_REG64(r9, amd64_r9);
        CHECK_REG64(r10, amd64_r10);
        CHECK_REG64(r11, amd64_r11);
        CHECK_REG64(r12, amd64_r12);
        CHECK_REG64(r13, amd64_r13);
        CHECK_REG64(r14, amd64_r14);
        CHECK_REG64(r15, amd64_r15);
        CHECK((qword_t) regs.rip, cpu->amd64_rip, "rip");
    } else {
        CHECK_REG(rax, eax);
        CHECK_REG(rbx, ebx);
        CHECK_REG(rcx, ecx);
        CHECK_REG(rdx, edx);
        CHECK_REG(rsi, esi);
        CHECK_REG(rdi, edi);
        CHECK_REG(rsp, esp);
        CHECK_REG(rbp, ebp);
        CHECK_REG(rip, eip);
    }
    undefined_flags |= (1 << 8); // treat trap flag as undefined
    regs.eflags = (regs.eflags & ~undefined_flags) | (cpu->eflags & undefined_flags);
    // give a nice visual representation of the flags
    if (regs.eflags != cpu->eflags) {
#define f(x,n) ((regs.eflags & (1 << n)) ? #x : "-"),
        printf("real eflags = 0x%llx %s%s%s%s%s%s%s%s%s, fake eflags = 0x%x %s%s%s%s%s%s%s%s%s\r\n%0d",
                regs.eflags, f(o,11)f(d,10)f(i,9)f(t,8)f(s,7)f(z,6)f(a,4)f(p,2)f(c,0)
#undef f
#define f(x,n) ((cpu->eflags & (1 << n)) ? #x : "-"),
                cpu->eflags, f(o,11)f(d,10)f(i,9)f(t,8)f(s,7)f(z,6)f(a,4)f(p,2)f(c,0)0);
        if (trap_wanted()) debugger;
        return -1;
    }

    for (int i = 0; i < 8; i++) {
        CHECK(*(uint64_t *) &fpregs.xmm_space[i * 4], cpu->xmm[i].qw[0], "xmm%d low", i);
        CHECK(*(uint64_t *) &fpregs.xmm_space[i*4+2], cpu->xmm[i].qw[1], "xmm%d high", i);
    }

#define FSW_MASK 0x7d00 // only look at top, c0, c2, c3
    CHECK(fpregs.swd & FSW_MASK, cpu->fsw & FSW_MASK, "fsw");
    CHECK(fpregs.cwd, cpu->fcw, "fcw");
    fpregs.swd &= FSW_MASK;
    for (int i = 0; i < 8; i++) {
        int ii = (cpu->top + i) % 8;
        uint64_t mm = cpu->mm[ii].qw;
        uint64_t f_signif =  cpu->fp[ii].signif;
        uint64_t expected = *(uint64_t *) &fpregs.st_space[i * 4];
        if (f_signif != expected && mm != expected) {
            reportf("mm/st(%d) signif: real %#llx, fake fp %#llx, fake mm %#llx\n", i, (unsigned long long) expected, (unsigned long long) f_signif, (unsigned long long) mm);
            if (trap_wanted()) debugger;
            return -1;
        }
        if (f_signif == expected && mm != expected) {
            CHECK(*(uint16_t *) &fpregs.st_space[i*4+2], cpu->fp[ii].signExp, "st(%d) sign/exp", i);
        }
    }

    // compare pages marked dirty
    if (tlb->dirty_page != TLB_PAGE_EMPTY) {
        int fd = open_mem(pid);
        page_t dirty_page = tlb->dirty_page;
        char real_page[PAGE_SIZE];
        trycall(lseek(fd, dirty_page, SEEK_SET), "compare seek mem");
        trycall(read(fd, real_page, PAGE_SIZE), "compare read mem");
        close(fd);
        struct pt_entry entry = *mem_pt(current->mem, PAGE(dirty_page));
        void *fake_page = entry.data->data + entry.offset;

        if (memcmp(real_page, fake_page, PAGE_SIZE) != 0) {
            reportf("page %x doesn't match\n", dirty_page);
            if (trap_wanted()) debugger;
            return -1;
        }
        tlb->dirty_page = TLB_PAGE_EMPTY;
    }

    setregs(pid, &regs);
    trycall(ptrace(PTRACE_SETFPREGS, pid, NULL, &fpregs), "ptrace setregs compare");
    return 0;
}

// I'd like to apologize in advance for this code
static int transmit_fd(int pid, int sender, int receiver, int fake_fd) {
    // this sends the fd over a unix domain socket. yes, I'm crazy

    // sending part
    int real_fd = f_get(fake_fd)->real_fd;
    struct msghdr msg = {};
    char cmsg[CMSG_SPACE(sizeof(int))];
    memset(cmsg, 0, sizeof(cmsg));

    msg.msg_control = cmsg;
    msg.msg_controllen = sizeof(cmsg);

    struct cmsghdr *cmsg_hdr = CMSG_FIRSTHDR(&msg);
    cmsg_hdr->cmsg_level = SOL_SOCKET;
    cmsg_hdr->cmsg_type = SCM_RIGHTS;
    cmsg_hdr->cmsg_len = CMSG_LEN(sizeof(int));
    *(int *) CMSG_DATA(cmsg_hdr) = real_fd;

    trycall(sendmsg(sender, &msg, 0), "sendmsg insanity");

    // receiving part
    // painful, because we're 64-bit and the child is 32-bit and I want to kill myself
    struct user_regs_struct saved_regs;
    getregs(pid, &saved_regs);
    struct user_regs_struct regs = saved_regs;

    // reserve space for 32-bit version of cmsg
    regs.rsp -= 16; // according to my calculations
    addr_t cmsg_addr = regs.rsp;
    char cmsg_bak[16];
    pt_readn(pid, regs.rsp, cmsg_bak, sizeof(cmsg_bak));

    // copy 32-bit msghdr
    regs.rsp -= 32;
    int msg32[] = {0, 0, 0, 0, cmsg_addr, 20, 0};
    addr_t msg_addr = regs.rsp;
    char msg_bak[32];
    pt_readn(pid, regs.rsp, msg_bak, sizeof(msg_bak));
    pt_writen(pid, regs.rsp, &msg32, sizeof(msg32));

    regs.rax = 372;
    regs.rbx = receiver;
    regs.rcx = msg_addr;
    regs.rdx = 0;
    // assume we're already on an int $0x80
    setregs(pid, &regs);
    step(pid);
    getregs(pid, &regs);

    int sent_fd;
    if ((long) regs.rax >= 0)
        pt_readn(pid, cmsg_addr + 12, &sent_fd, sizeof(sent_fd));
    else
        sent_fd = regs.rax;

    // restore crap
    pt_writen(pid, cmsg_addr, cmsg_bak, sizeof(cmsg_bak));
    pt_writen(pid, msg_addr, msg_bak, sizeof(msg_bak));
    setregs(pid, &regs);

    if (sent_fd < 0) {
        errno = -sent_fd;
        perror("remote recvmsg insanity");
        exit(1);
    }

    return sent_fd;
}

static void remote_close_fd(int pid, int fd, long int80_ip) {
    // lettuce spray
    struct user_regs_struct saved_regs;
    getregs(pid, &saved_regs);
    struct user_regs_struct regs = saved_regs;
    regs.rip = int80_ip;
    regs.rax = 6;
    regs.rbx = fd;
    setregs(pid, &regs);
    step(pid);
    getregs(pid, &regs);
    if ((long) regs.rax < 0) {
        errno = -regs.rax;
        perror("remote close fd");
        exit(1);
    }
    setregs(pid, &regs);
}

#define _ignore(x) {}; int UNUSED(x) =
#define ignore _ignore(__COUNTER__)

static void pt_copy(int pid, addr_t start, size_t size) {
    if (start == 0)
        return;
    byte_t byte;
    for (addr_t addr = start; addr < start + size; addr++) {
        ignore user_get(addr, byte);
        pt_write8(pid, addr, byte);
    }
}

// Please don't use unless absolutely necessary.
static void pt_copy_to_real(int pid, addr_t start, size_t size) {
    byte_t byte;
    for (addr_t addr = start; addr < start + size; addr++) {
        pt_readn(pid, addr, &byte, sizeof(byte));
        ignore user_put(addr, byte);
    }
}

// amd64 syscall interception during single-stepping. Returns true if the syscall
// must execute on the real cpu (it changes the address space / TLS / signal
// state), false if ptraceomatic should substitute the fake cpu's result (rax).
// Memory-result copying for individual syscalls is added incrementally.
static bool amd64_intercept_syscall(struct cpu_state *cpu, int pid, struct user_regs_struct *regs, int sender, int receiver, long *saved_fd) {
    (void) pid; (void) regs; (void) sender; (void) receiver; (void) saved_fd;
    switch (cpu->amd64_regs[amd64_rax]) {
        case 9:   // mmap
        case 10:  // mprotect
        case 11:  // munmap
        case 12:  // brk
        case 13:  // rt_sigaction
        case 14:  // rt_sigprocmask
        case 15:  // rt_sigreturn
        case 158: // arch_prctl (sets fs base for TLS)
            return true;
    }
    return false;
}

static void step_tracing(struct cpu_state *cpu, struct tlb *tlb, int pid, int sender, int receiver) {
    // step fake cpu
    bool is_amd64 = current->abi == GUEST_ABI_AMD64;
    cpu->tf = 1;
    uint64_t pre_eip = is_amd64 ? cpu->amd64_rip : cpu->eip;
    int interrupt = cpu_run_to_interrupt(cpu, tlb);
    // hack to clean up before the exit syscall
    if (interrupt == INT_SYSCALL &&
            (is_amd64 ? (cpu->amd64_regs[amd64_rax] == 60 || cpu->amd64_regs[amd64_rax] == 231)
                      : (cpu->eax == 1))) {
        if (kill(pid, SIGKILL) < 0) {
            perror("kill tracee during exit");
            exit(1);
        }
    }
    if (interrupt != INT_DEBUG) {
        handle_interrupt(interrupt);
        // A fault the emulator resolves itself -- growing the stack, faulting in
        // a page -- leaves the faulting instruction UN-RETIRED, to be retried on
        // the next run. Stepping the real CPU anyway puts the two one instruction
        // out of phase, and the next compare then blames whatever innocent
        // instruction happens to be next. The syscall interrupts are not this
        // case: handle_interrupt runs the syscall and advances past it, and the
        // interception below needs to see the int $0x80 in the real process.
        int syscall_int = is_amd64 ? INT_AMD64_SYSCALL : INT_SYSCALL;
        uint64_t post_eip = is_amd64 ? cpu->amd64_rip : cpu->eip;
        if (interrupt != syscall_int && post_eip == pre_eip) {
            // If handle_interrupt cannot make progress, holding the real CPU
            // back forever turns a false divergence report into a silent hang,
            // which is worse. Bound it and say so instead.
            static uint64_t held_eip;
            static int held_times;
            held_times = (pre_eip == held_eip) ? held_times + 1 : 0;
            held_eip = pre_eip;
            if (held_times >= 16) {
                reportf("ptraceomatic: fake cpu stuck on interrupt %d at eip %#llx, "
                        "not retiring it after %d attempts\n",
                        interrupt, (unsigned long long) pre_eip, held_times);
                exit(1);
            }
            if (trace_syscalls())
                reportf("ptraceomatic: fake cpu took interrupt %d at eip %#llx without retiring it; "
                        "holding the real cpu back a step\n", interrupt, (unsigned long long) pre_eip);
            return;
        }
    }

    // step real cpu
    // intercept cpuid, rdtsc, and int $0x80, though
    struct user_regs_struct regs;
    errno = 0;
    getregs(pid, &regs);
    long inst = trycall(ptrace(PTRACE_PEEKTEXT, pid, regs.rip, NULL), "ptrace get inst step");
    long saved_fd = -1; // annoying hack for mmap
    long old_sp = regs.rsp; // so we know where a sigframe ends

    if ((inst & 0xff) == 0x0f) {
        if (((inst & 0xff00) >> 8) == 0xa2) {
            // cpuid
            do_cpuid((dword_t *) &regs.rax, (dword_t *) &regs.rbx, (dword_t *) &regs.rcx, (dword_t *) &regs.rdx);
            regs.rip += 2;
        } else if (((inst & 0xff00) >> 8) == 0x31) {
            // rdtsc, no good way to get the same result here except copy from fake cpu
            if (is_amd64) {
                regs.rax = cpu->amd64_regs[amd64_rax];
                regs.rdx = cpu->amd64_regs[amd64_rdx];
            } else {
                regs.rax = cpu->eax;
                regs.rdx = cpu->edx;
            }
            regs.rip += 2;
        } else if (is_amd64 && ((inst & 0xff00) >> 8) == 0x05) {
            // syscall (amd64): take the fake cpu's result, or run it for real
            if (amd64_intercept_syscall(cpu, pid, &regs, sender, receiver, &saved_fd))
                goto do_step;
            regs.rax = cpu->amd64_regs[amd64_rax];
            regs.rip += 2;
        } else {
            goto do_step;
        }
    } else if (!is_amd64 && (inst & 0xff) == 0xcd && ((inst & 0xff00) >> 8) == 0x80) {
        // int $0x80, intercept the syscall unless it's one of a few actually important ones
        dword_t syscall_num = (dword_t) regs.rax;
        bool synced_memory = true;
        switch (syscall_num) {
            // put syscall result from fake process into real process
            case 3: // read
                pt_copy(pid, regs.rcx, cpu->edx); break;
            case 7: // waitpid
                pt_copy(pid, regs.rcx, sizeof(dword_t)); break;
            case 13: // time
                if (regs.rbx != 0)
                    pt_copy(pid, regs.rbx, sizeof(dword_t));
                break;
            case 43:
                pt_copy(pid, regs.rbx, sizeof(struct tms_)); break;
            case 54: { // ioctl (god help us)
                struct fd *fd = f_get(cpu->ebx);
                if (fd && fd->ops->ioctl_size) {
                    ssize_t ioctl_size = fd->ops->ioctl_size(cpu->ecx);
                    if (ioctl_size >= 0)
                        pt_copy(pid, regs.rdx, ioctl_size);
                }
                break;
            }
            case 85: // readlink
                pt_copy(pid, regs.rcx, regs.rdx); break;
            case 102: { // socketcall
                dword_t args[6];
                ignore user_get(regs.rcx, args);
                dword_t len;
                switch (cpu->ebx) {
                    case 6: // getsockname
                        ;ignore user_get(args[2], len);
                        pt_copy(pid, args[1], len);
                        break;
                    case 8: // socketpair
                        pt_copy(pid, args[3], sizeof(dword_t[2]));
                        break;
                    case 12: // recvfrom
                        pt_copy(pid, args[1], args[2]);
                        ignore user_get(args[5], len);
                        pt_copy(pid, args[4], len);
                        break;
                }
                break;
            }
            case 104: // setitimer
                pt_copy(pid, regs.rdx, sizeof(struct itimerval_)); break;
            case 116: // sysinfo
                pt_copy(pid, regs.rbx, sizeof(struct sys_info)); break;
            case 122: // uname
                pt_copy(pid, regs.rbx, sizeof(struct uname)); break;
            case 140: // _llseek
                pt_copy(pid, regs.rsi, 8); break;
            case 145: { // readv
                struct iovec_ vecs[regs.rdx];
                ignore user_get(regs.rcx, vecs);
                for (unsigned i = 0; i < regs.rdx; i++)
                    pt_copy(pid, vecs[i].base, vecs[i].len);
                break;
            }
            case 162: // nanosleep
                pt_copy(pid, regs.rcx, sizeof(struct timespec_)); break;
            case 168: // poll
                pt_copy(pid, regs.rbx, sizeof(struct pollfd_) * regs.rcx); break;
            case 183: // getcwd
                pt_copy(pid, regs.rbx, cpu->eax); break;
            case 186: // sigaltstack
                if (regs.rcx != 0) pt_copy(pid, regs.rcx, sizeof(struct stack_t_));
                break;
            case 76:  // old_getrlimit
            case 191: // ugetrlimit
                pt_copy(pid, regs.rcx, sizeof(struct rlimit32_)); break;
            case 195: // stat64
            case 196: // lstat64
            case 197: // fstat64
                pt_copy(pid, regs.rcx, sizeof(struct newstat64)); break;
            case 220: // getdents64
                pt_copy(pid, regs.rcx, cpu->eax); break;
            case 242: // sched_getaffinity
                pt_copy(pid, regs.rdx, regs.rcx); break;
            case 265: // clock_gettime
                pt_copy(pid, regs.rcx, sizeof(struct timespec_)); break;
            case 300: // fstatat64
                pt_copy(pid, regs.rdx, sizeof(struct newstat64)); break;
            case 305: // readlinkat
                if (cpu->eax < 0xffff000) pt_copy(pid, regs.rdx, cpu->eax);
                break;
            case 340: // prlimit
                if (regs.rsi != 0) pt_copy(pid, regs.rsi, sizeof(struct rlimit_));
                break;
            case 355: // getrandom
                pt_copy(pid, regs.rbx, regs.rcx); break;

            case 90: // mmap
            case 192: // mmap2
                if (cpu->eax < 0xfffff000 && cpu->edi != (dword_t) -1) {
                    // fake mmap didn't fail, change fd
                    saved_fd = regs.rdi;
                    regs.rdi = transmit_fd(pid, sender, receiver, cpu->edi);
                }
                goto do_step;

            // some syscalls need to just happen
            case 45: // brk
            case 91: // munmap
            case 119: // sigreturn
            case 125: // mprotect
            case 173: // rt_sigreturn
            case 174: // rt_sigaction
            case 175: // rt_sigprocmask
            case 243: // set_thread_area
                //regs.rax = cpu->eax;
                goto do_step;

            default:
                synced_memory = false;
                break;
        }
        if (trace_syscalls())
            reportf("ptraceomatic: syscall %u -> %#x%s\n", (unsigned) syscall_num,
                    (unsigned) cpu->eax, synced_memory ? "" : "   [no memory sync]");
        regs.rax = cpu->eax;
        regs.rip += 2;
    } else {
do_step:
        setregs(pid, &regs);
        // single step on a repeated string instruction only does one
        // iteration, so loop until ip changes
        unsigned long ip = regs.rip;
        int was_signal;
        while (regs.rip == ip) {
            was_signal = step(pid);
            getregs(pid, &regs);
        }
        if (saved_fd >= 0) {
            remote_close_fd(pid, regs.rdi, ip);
            regs.rdi = saved_fd;
        }

        if (was_signal) {
            // copy the return address
            pt_copy(pid, regs.rsp, sizeof(addr_t));
            // and copy the rest the other way
            pt_copy_to_real(pid, regs.rsp + sizeof(addr_t), old_sp - regs.rsp - sizeof(addr_t));
        }
    }
    setregs(pid, &regs);
}

// The tracee dying during setup used to surface only as a bare
// "ptrace getregs: No such process" from whatever call happened to run next,
// with no indication of which setup step killed it. kill(pid, 0) is not enough:
// a tracee that has exited but not been reaped is a zombie, which still accepts
// signal 0, so read the state out of /proc instead.
static void check_tracee_alive(int pid, const char *where) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "ptraceomatic: tracee vanished during %s\n", where);
        exit(1);
    }
    char line[256];
    char state = '?';
    while (fgets(line, sizeof(line), f) != NULL)
        if (strncmp(line, "State:", 6) == 0) {
            sscanf(line, "State:\t%c", &state);
            break;
        }
    fclose(f);
    if (state == 'Z' || state == 'X') {
        fprintf(stderr, "ptraceomatic: tracee died during %s (state %c)\n", where, state);
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            if (WIFEXITED(status))
                fprintf(stderr, "  exited with status %d\n", WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                fprintf(stderr, "  killed by signal %d\n", WTERMSIG(status));
        }
        exit(1);
    }
}

static void prepare_tracee(int pid) {
    struct user_regs_struct regs;
    check_tracee_alive(pid, "entry to prepare_tracee");
    if (current->abi == GUEST_ABI_AMD64) {
        // match the emulator's initial rsp/rip. (vdso + argv/auxv stack sync for
        // programs that actually read them is amd64 phase-2 work.)
        getregs(pid, &regs);
        regs.rsp = current->cpu.amd64_regs[amd64_rsp];
        regs.rip = current->cpu.amd64_rip;
        setregs(pid, &regs);
    } else {
        check_tracee_alive(pid, "exec");
        transplant_vdso(pid, vdso_data, sizeof(vdso_data));
        check_tracee_alive(pid, "vdso transplant");
        // Copy the initial stack into the tracee. This used to hardcode
        // 0xffffd000/0x1000, which still happens to be the right range for the
        // i386 layout -- but only by coincidence, since the layout owns those
        // addresses and has several variants. Derive them so a layout change
        // cannot silently start copying the wrong page.
        struct guest_vm_layout layout = guest_abi_vm_layout(current->abi);
        addr_t stack_top = (addr_t) layout.stack_pointer;
        addr_t stack_first = current->cpu.esp & ~(addr_t) (PAGE_SIZE - 1);
        if (stack_first >= stack_top) {
            // esp is already at or above the top (nothing pushed yet): fall
            // back to the single page the layout reserves for the stack.
            stack_first = (addr_t) layout.stack_page << PAGE_BITS;
        }
        pt_copy(pid, stack_first, stack_top - stack_first);
        check_tracee_alive(pid, "stack copy");
        getregs(pid, &regs);
        regs.rsp = current->cpu.esp;
        setregs(pid, &regs);
    }

    // find out how big the signal stack frame needs to be
    __asm__("cpuid"
            : "=b" (xsave_extra)
            : "a" (0xd), "c" (0)
            : "edx");

    int features_ecx, features_edx;
    __asm__("cpuid"
            : "=c" (features_ecx), "=d" (features_edx)
            : "a" (1)
            : "ebx");
    // if xsave is supported, add 4 bytes. why? idk
    if (features_ecx & (1 << 26))
        xsave_extra += 4;
    // if fxsave/fxrestore is supported, use 112 bytes for that
    if (features_edx & (1 << 24))
        fxsave_extra = 112;

}

int main(int argc, char *const argv[]) {
    char *envp;
    char *term = getenv("TERM");
    if (term) {
        size_t term_len = strlen(term);
        // "TERM=" + term + "\0" + "\0"
        envp = malloc(5 + term_len + 2);
        if (!envp) {
            fprintf(stderr, "malloc: %s\n", strerror(ENOMEM));
            return -ENOMEM;
        }
        snprintf(envp, 5 + term_len + 1, "TERM=%s", term);
        envp[5 + term_len + 1] = '\0'; // double null terminator
    } else {
        envp = malloc(1);
        if (!envp) {
            fprintf(stderr, "malloc: %s\n", strerror(ENOMEM));
            return -ENOMEM;
        }
        envp[0] = '\0';
    }

    int err = xX_main_Xx(argc, argv, envp);
    free(envp);
    if (err < 0) {
        fprintf(stderr, "%s\n", strerror(-err));
        return err;
    }

    // execute the traced program in a new process and throw up some sockets
    char exec_path[MAX_PATH];
    if (path_normalize(AT_PWD, argv[optind], exec_path, N_SYMLINK_FOLLOW) != 0) {
        fprintf(stderr, "enametoolong\n"); exit(1);
    }
    struct mount *mount = find_mount_and_trim_path(exec_path);
    int fds[2];
    trycall(socketpair(AF_UNIX, SOCK_DGRAM, 0, fds), "socketpair");
    int pid = start_tracee(mount->root_fd, fix_path(exec_path), argv + optind, (char *[]) {NULL});
    int sender = fds[0], receiver = fds[1];
    /* close(receiver); // only needed in the child */
    prepare_tracee(pid);
    check_tracee_alive(pid, "prepare_tracee");

    struct cpu_state *cpu = &current->cpu;
    cpu->tf = true;
    struct tlb tlb;
    tlb_refresh(&tlb, cpu->mmu);
    int undefined_flags = 2;
    struct cpu_state old_cpu = *cpu;
    int i = 0;
    check_tracee_alive(pid, "pre-loop");
    while (true) {
        if (compare_cpus(cpu, &tlb, pid, undefined_flags) < 0) {
            // Resetting and re-running the instruction is a debugging aid: it
            // is useful when a person is sitting in gdb at the int3 above and
            // wants to step the same instruction again. Without a debugger it
            // is an infinite loop printing the same divergence, so the default
            // is to report once and stop with a status that says so.
            // The instruction that did it is the one just stepped, so its
            // address is the eip we saved BEFORE the step. Without this the
            // report says two registers differ and leaves you to find out
            // where, which for a divergence 422 instructions into libc start-up
            // is most of the work.
            reportf("ptraceomatic: emulated and real CPU diverged after %d instruction(s)\n", i);
            reportf("  last instruction at eip 0x%x, now at 0x%x\n",
                    old_cpu.eip, cpu->eip);
            reportf("  bytes at 0x%x:", old_cpu.eip);
            for (addr_t a = old_cpu.eip; a < old_cpu.eip + 12; a += sizeof(dword_t)) {
                dword_t word = pt_read(pid, a);
                for (unsigned b = 0; b < sizeof(word); b++)
                    reportf(" %02x", (unsigned) ((word >> (b * 8)) & 0xff));
            }
            reportf("\n");
            if (!trap_wanted()) {
                reportf("ptraceomatic: set PTRACEOMATIC_TRAP=1 to break into a debugger here\n");
                return 1;
            }
            do {
                reportf("failure: resetting cpu\n");
                *cpu = old_cpu;
                debugger;
                cpu_run_to_interrupt(cpu, &tlb);
            } while (compare_cpus(cpu, &tlb, pid, undefined_flags) < 0);
        }
        undefined_flags = undefined_flags_mask(cpu, &tlb);
        old_cpu = *cpu;
        step_tracing(cpu, &tlb, pid, sender, receiver);
        i++;
    }
}

// useful for calling from the debugger
void dump_memory(int pid, const char *file, addr_t start, addr_t end) {
    FILE *f = fopen(file, "w");
    for (addr_t addr = start; addr <= end; addr += sizeof(dword_t)) {
        dword_t val = pt_read(pid, addr);
        fwrite(&val, sizeof(dword_t), 1, f);
    }
    fclose(f);
}
