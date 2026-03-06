## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".

## 2026-03-06 - Buffer Overflow in Command-Line Arguments Parsing
**Vulnerability:** In `xX_main_Xx.h`, command line arguments (`argv`) are flattened into a stack-allocated 4096-byte buffer `argv_copy`. The loop copies each argument string via `memcpy` and increments an index `p`, but there was no check to ensure `p + arg_len` did not exceed the size of the buffer. An attacker providing many large arguments could trigger a stack-based buffer overflow, overwriting return addresses or neighboring stack variables.
**Learning:** Even simple setup routines or loop-based string copies must enforce length bounds on the destination buffer. Assuming `argv` size is inherently limited (or smaller than 4K) is unsafe, as user-provided arguments can easily exceed this limit.
**Prevention:** Always maintain and check the remaining capacity in destination buffers during loop-based concatenations or copies. Return a clear error like `_E2BIG` (Argument list too long) when boundaries are exceeded.
