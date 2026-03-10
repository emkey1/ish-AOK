## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".

## 2026-02-24 - Massive Kernel Stack Allocation
**Vulnerability:** A massive constant `ARGV_MAX` (128 KB) was used to size a static array (`char new_argv_buf[ARGV_MAX];`) allocated on the stack in `kernel/exec.c`'s `shebang_exec`. In kernel-space with small stack limits, this causes a stack overflow (DoS / crash vulnerability).
**Learning:** Massive macro constants defined as bounds limits should never be mapped 1:1 to stack array allocations without checking their actual size.
**Prevention:** For large allocations (e.g. anything over a few KB), always use heap allocation (`malloc`), check for `NULL`, and `free` correctly on all execution paths.
