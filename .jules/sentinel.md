## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".

## 2026-02-24 - Kernel Stack DoS via Large Stack Allocations
**Vulnerability:** A local variable `char new_argv_buf[ARGV_MAX];` allocated 128KB on the kernel stack in `shebang_exec` (`kernel/exec.c`). Given strict kernel stack limits, an attacker could trigger a stack overflow by executing a shebang script, leading to a Denial of Service (DoS) or potential arbitrary code execution within the kernel environment.
**Learning:** Massive constants like `ARGV_MAX` (128KB) should never be used to allocate arrays on the stack due to tight stack limitations, even for short-lived operations.
**Prevention:** Large buffers must be dynamically allocated on the heap using `malloc()` with proper `NULL` checks, and they must be freed on all execution paths to prevent memory leaks while avoiding stack overflow DoS vulnerabilities.

## 2026-03-17 - Thread Name Data Race and Unprotected Write
**Vulnerability:** In `kernel/misc.c`, the `sys_prctl` syscall updated `current->comm` (the thread name) via `strcpy` without holding the necessary lock (`current->general_lock`), which could lead to data races with concurrent readers.
**Learning:** Thread name updates must always be protected by the `general_lock` and should ideally use bounded copy functions like `strncpy` for defense-in-depth, even if the input string is known to be null-terminated.
**Prevention:** Ensure that access to `current->comm` is properly synchronized across all syscalls and kernel paths. Use `strncpy` to prevent accidental buffer overflows if the target buffer size ever changes.

## 2026-03-24 - Buffer Overflow in get_filesystems
**Vulnerability:** The `get_filesystems` function in `fs/mount.c` used a fixed-size buffer (`calloc(MAX_FILESYSTEMS * 50)`) and consecutive `strcat` calls. If the names of registered filesystems were large enough, this would result in a heap buffer overflow.
**Learning:** Fixed-size buffers with string concatenations in a loop are inherently prone to overflows, especially if the loop iterations or string lengths can grow over time. Even if seemingly "reasonable", they are fragile and create technical debt.
**Prevention:** Use a two-pass approach for dynamic string accumulation: first, iterate to calculate the exact total length required; second, allocate the exact memory (`malloc` with a `NULL` check) and populate it (e.g., using pointer advancement and `memcpy`). This eliminates `strcat` entirely and ensures memory safety without arbitrary limitations.

## 2026-03-24 - Buffer Overflow in Command Line Arguments Copy
**Vulnerability:** In `xX_main_Xx.h`, the array `argv_copy` was statically sized at 4096 bytes. The arguments loop used `memcpy` to copy user-supplied inputs from `argv` directly into `argv_copy` based strictly on `strlen(argv[i])` without ensuring the total size remained within 4096 bytes. This allowed a stack buffer overflow by passing large command-line arguments.
**Learning:** Hardcoded stack buffer limits with unchecked string accumulations are a critical vulnerability vector, especially for command-line arguments which are easily controlled by external actors.
**Prevention:** Always implement explicit bounds checking before performing string copies or concatenations into stack-allocated buffers. Return appropriate errors (e.g., `_E2BIG`) if the boundary is exceeded.

## 2026-03-25 - Buffer Overflow in getpath format
**Vulnerability:** `fs/real.c` used a 20-byte buffer for formatting `/proc/self/fd/%d`, but the path prefix is 14 bytes and a 32-bit signed int can be 11 bytes, requiring at least 26 bytes. This allows a stack buffer overflow for very large file descriptors.
**Learning:** Hardcoded buffer sizes for path formatting often fail to account for the maximum string length of integers.
**Prevention:** Always allocate at least 32 bytes for paths containing PIDs or FDs, and consider using `snprintf` to avoid overflow entirely.
## $(date +%Y-%m-%d) - Replace unsafe strcpy calls with strncpy in fs/tmp.c
**Vulnerability:** Unbounded string copies (`strcpy`) writing to fixed-size char arrays (`char name[MAX_NAME + 1]`) in `fs/tmp.c`.
**Learning:** Legacy VFS and temporary filesystem code often lacks explicit bounds checking, relying on higher-level path validation. This creates defense-in-depth vulnerabilities if `MAX_NAME` bounds are ever exceeded.
**Prevention:** Always use `strncpy` and manually ensure null-termination for fixed-size string arrays in the kernel, regardless of upstream path validation.
## 2026-03-26 - Buffer Overflow DoS via TERM Environment Variable
**Vulnerability:** A fixed-size stack buffer (`char envp[100]`) in `main.c` and `tools/ptraceomatic.c` was vulnerable to a buffer overflow when constructing the `TERM` environment variable. A user could trigger a Denial of Service (DoS) or stack corruption by supplying a `TERM` string exceeding the 100-character stack limit.
**Learning:** Hardcoding stack buffer limits for dynamically sized user inputs (like environment variables) creates critical security risks and should be dynamically allocated instead.
**Prevention:** Always use safe construction methods (e.g. `snprintf` with `malloc`) when passing dynamically-sized string inputs into kernel or environment initialization bounds, verifying explicitly free routines.
