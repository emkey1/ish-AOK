## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".

## 2024-05-18 - Fix stack buffer overflow in argv copy
**Vulnerability:** In `xX_main_Xx.h`, command-line arguments (`argv`) were iteratively copied into a fixed-size 4096-byte stack-allocated buffer (`argv_copy`) using a `while` loop, without verifying if the accumulated argument size exceeded the buffer's capacity. This lack of bounds checking could lead to a stack buffer overflow.
**Learning:** Stack buffers used for collecting sequential user input or dynamic variables require explicit bounds checking at each iteration to ensure the total size does not exceed the buffer limits, avoiding potential memory corruption and control flow hijacking.
**Prevention:** Always track the current offset and enforce bounds checks (e.g., `if (p + arg_len > sizeof(argv_copy) - 1)`) within loops that accumulate variable-length string data into fixed-size stack buffers. Return appropriate system error codes like `_E2BIG` when limits are exceeded.
