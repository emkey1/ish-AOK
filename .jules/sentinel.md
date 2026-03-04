## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".

## 2026-03-04 - Buffer Overflow in Command Line Arguments Parsing
**Vulnerability:** In `xX_main_Xx.h`, the variable `argv_copy` was a fixed-size stack buffer of 4096 bytes used to flatten the command-line arguments array. The implementation repeatedly appended to this buffer via `memcpy` without verifying that the total size remained within the buffer limits.
**Learning:** Functions that parse unbounded external inputs like command line parameters directly into static stack buffers often harbor simple stack buffer overflow vulnerabilities. Such inputs, while "trusted" in typical setups, can be exploited or cause crashes if artificially long strings are passed.
**Prevention:** Always implement explicit bounds checking before sequentially writing to fixed-size buffers, particularly when combining or flattening arrays. Return a clear error like `_E2BIG` (Argument list too long) rather than truncating or overflowing.
