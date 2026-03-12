## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".

## 2026-03-12 - Stack Buffer Overflow in Command-Line Arguments
**Vulnerability:** In `xX_main_Xx.h`, the loop copying command-line arguments into the fixed-size 4096-byte `argv_copy` stack buffer lacked bounds checking. An adversary or script passing sufficiently long arguments could trigger a stack buffer overflow.
**Learning:** Even internal initialization logic and argument passing functions are susceptible to bounds checking failures if they rely on stack-allocated arrays for large, potentially variable data like `argv`.
**Prevention:** Always perform explicit bounds checking when copying string arrays or variable-length data into fixed-size stack buffers, returning an error like `_E2BIG` (Argument list too long) if the buffer size limit is exceeded.
