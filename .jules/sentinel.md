## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".

## 2024-05-24 - [Fix stack buffer overflow in argv parsing]
**Vulnerability:** A critical stack buffer overflow vulnerability existed in `xX_main_Xx.h` during argument parsing (`argv_copy`). The while loop copied unbounded command-line arguments into a fixed 4096-byte stack array without bounds checking.
**Learning:** Hardcoded stack buffer limits can be easily bypassed by large user inputs (e.g., passing numerous large command-line arguments), leading to stack corruption, crashes, and potentially arbitrary code execution.
**Prevention:** Always validate the length of user inputs against the target buffer size before copying data. Use exact boundary checks (e.g., `if (p + arg_len > sizeof(argv_copy) - 1)`) and return appropriate error codes (e.g., `_E2BIG`) when limits are exceeded.
