## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".

## $(date +%Y-%m-%d) - Fix buffer overflow in ptrace util via sprintf
**Vulnerability:** A potential buffer overflow vulnerability existed in `tools/ptutil.c` where `sprintf` was used to write a process ID into a statically sized string buffer without bounds checking.
**Learning:** Although `char filename[1024]` provides a large buffer, using `sprintf` without length restrictions leaves the codebase vulnerable if integer values unexpectedly become large or inputs are maliciously manipulated.
**Prevention:** Always use `snprintf` with `sizeof(buffer)` when formatting strings into statically allocated char arrays in C, even when the buffer appears sufficiently large for expected inputs.
