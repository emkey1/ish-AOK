## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".

## 2026-02-25 - Stack Buffer Overflow via Directory Listing
**Vulnerability:** A missing bounds check in `fakefs_readdir` allowed `entry_path` (a 4096-byte stack buffer) to overflow when encountering directory entries whose path length exceeded `MAX_PATH`. The original code literally commented "god I don't know what to do if this would overflow" instead of handling it.
**Learning:** String concatenations (`strcat`, `strcpy`) into fixed-size stack buffers must always be verified against the buffer capacity. Unhandled edge cases documented with comments can be straightforward indicators of existing vulnerabilities.
**Prevention:** Explicitly use bounds checking (`strlen(path) + 1 + strlen(name) >= MAX_PATH`) prior to any concatenation, and safely skip or abort the processing of oversized inputs (e.g. using `goto retry` to skip to the next directory entry).
