## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".

## $(date +%Y-%m-%d) - Fix stack buffer overflow in `xX_main_Xx.h`
**Vulnerability:** A `memcpy` call in `xX_main_Xx` was copying command line arguments into a fixed 4096 byte stack buffer `argv_copy` without checking the remaining buffer space. If the arguments were larger than 4096 bytes, a stack buffer overflow would occur.
**Learning:** Command-line argument copying in `xX_main_Xx.h` into the `argv_copy` stack buffer requires explicit bounds checking against its 4096-byte limit to prevent stack buffer overflows from `argv`.
**Prevention:** Always verify `len < max_size` when appending or copying dynamically sized strings to static buffers. Return standard error code like `_E2BIG` (Argument list too long) when boundaries are exceeded.
