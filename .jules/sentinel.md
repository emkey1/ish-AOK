## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".

## 2026-03-15 - Stack Buffer Overflow in argv_copy
**Vulnerability:** `xX_main_Xx.h` used a fixed-size `4096` byte stack buffer `argv_copy` to copy command-line arguments without checking if the combined length exceeded the buffer capacity. This could lead to a stack buffer overflow by passing excessively large arguments.
**Learning:** Hardcoded stack buffer limits can be easily bypassed by user input. Large buffers like `ARGV_MAX` or any sizable stack arrays must be explicitly bounded or replaced with heap allocation for robustness and security against DoS or arbitrary execution.
**Prevention:** Always validate size and boundaries when copying unbounded user or external inputs into statically sized stack buffers. Return appropriate system error codes, like `_E2BIG`, when size limits are exceeded.
