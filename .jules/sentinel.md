## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".
## 2024-05-18 - Stack Buffer Overflow in Command-Line Arguments Parsing
**Vulnerability:** A local attacker could cause a stack buffer overflow by providing excessively long command-line arguments. The `argv_copy` stack buffer (4096 bytes) in `xX_main_Xx.h` was being filled sequentially with lengths from `argv[i]` without any bounds checking against its maximum capacity.
**Learning:** Even internal initialization and setup routines using `argv` must be validated against hardcoded stack limits, as missing bounds checks on fixed-size buffers directly expose kernel/application logic to simple DoS or memory corruption via CLI.
**Prevention:** Always validate that accumulated lengths (`p + arg_len`) do not exceed the buffer's safe maximum capacity (`sizeof(buf) - 1`) before appending to stack-allocated arrays, and return `_E2BIG` (Argument list too long) if limits are exceeded.
