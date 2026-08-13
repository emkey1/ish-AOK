#ifndef KERNEL_INIT_H
#define KERNEL_INIT_H

#include "fs/tty.h"

// Incredibly sloppy. Please do not reference as an example of good API design.
// `name` is what / reports as its source in /proc/mounts (df's "Filesystem"
// column); NULL reports the host path it was mounted from.
int mount_root(const struct fs_ops *fs, const char *source, const char *name);
void set_console_device(int major, int minor);
void get_console_device(int *major, int *minor);
intptr_t become_first_process(void);
intptr_t become_new_init_child(void);
int create_stdio(const char *file, int major, int minor);
int create_piped_stdio(void);

// Result of run_guest_command_capture(). output is malloc'd and must be freed
// by the caller (NULL only on allocation failure).
struct guest_command_result {
    int launched;       // 1 if /bin/sh was started, 0 if spawn/exec failed
    int exited;         // 1 if the command exited normally (exit_code valid)
    int exit_code;      // exit status when exited==1
    int term_signal;    // terminating signal when exited==0 (e.g. SIGKILL on timeout)
    int timed_out;      // 1 if killed because timeout_ms elapsed
    int truncated;      // 1 if output was capped at max_output
    char *output;       // merged stdout+stderr, NUL-terminated; caller frees
    size_t output_len;  // bytes in output (excluding the terminating NUL)
};

// Run `/bin/sh -c command` as a fresh child of init, capturing merged
// stdout+stderr into result->output, and wait for it to finish (or kill it
// after timeout_ms, 0 = no timeout). env is a packed NUL-separated, empty-string
// terminated environment block (NULL for a sensible default). max_output caps
// captured bytes (0 = 64 KiB default). MUST be called on a dedicated host
// thread, not a guest task thread: it temporarily repoints `current`.
// Returns 0 if the command was launched (inspect result for its outcome), or a
// negative errno if it could not be started.
int run_guest_command_capture(const char *command, const char *env,
                              int timeout_ms, size_t max_output,
                              struct guest_command_result *result);

#endif
