#ifndef KERNEL_NATIVE_H
#define KERNEL_NATIVE_H

#include <stddef.h>
#include "misc.h"

// Programs whose implementation is compiled into iSH-AOK and executed as HOST
// code, instead of being loaded and translated as guest instructions.
//
// They are reached through /AOK/native/<name> (fs/aok.c). Exec matches on the
// RESOLVED path, so a symlink from anywhere dispatches natively while argv[0]
// stays whatever the caller passed -- which is exactly what lets one multicall
// binary serve `ln -s /AOK/native/smallclue /usr/local/bin/df` the same way it
// works on Linux. Adding a program is a table entry here plus a node in
// fs/aok.c; the dispatcher itself does not change.
//
// Because no guest code runs, a native program is guest-ABI-independent: one
// implementation serves i386, amd64, arm64 and riscv64 guests alike, which
// matters most for the slowest of them.
//
// Execution model, and the two things easiest to get wrong:
//
//  - A native program is NOT a host process. It runs on the calling task's own
//    thread, inside the execve syscall that would otherwise have replaced the
//    process image, and its return value becomes that process's exit status.
//    iSH's existing task is the process the guest sees, so pid, waitpid and
//    exit status all work without new machinery. It also means a long-running
//    native program blocks that guest task for its whole duration.
//
//  - A native program is linked against the HOST libc. A bare open()/write()
//    would therefore hit the host filesystem, not the guest's rootfs. All I/O
//    must go through iSH's kernel instead (fd_write_host_buf and friends in
//    kernel/fs.h). This is the seam that real programs need filling in before
//    they can do anything filesystem-shaped.

struct native_program {
    // basename under /AOK/native/
    const char *name;
    // argv[0] is the name the caller invoked, as on any multicall binary.
    // envp is NULL-terminated. The return value becomes the exit status.
    int (*main)(int argc, char *const argv[], char *const envp[]);
};

// NULL when no native program of that name is compiled into this build. Exec
// then falls through to the ordinary path and runs the /AOK/native stub, which
// reports the situation loudly rather than failing with ENOEXEC.
const struct native_program *native_program_lookup(const char *name);

// Runs prog on the current task and terminates that task with prog's return
// value. Called from execve at the point the image would have been replaced,
// so it never returns.
noreturn void native_program_exec(const struct native_program *prog,
        int argc, char *const argv[], char *const envp[]);

#endif
