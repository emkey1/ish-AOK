## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".

## 2026-02-24 - Stack Buffer Overflow in Path Manipulation
**Vulnerability:** Several instances of `strcat` and `strcpy` were used in `fs/fake.c` and `fs/path.c` to append path components to a static buffer (`MAX_PATH` size) without verifying if the length of the concatenated strings would exceed the buffer's capacity. This allows for a stack-based buffer overflow if user-supplied paths are artificially constructed to exceed limits.
**Learning:** Checking string lengths against `MAX_PATH` is easy to get wrong with an off-by-one error. `strlen(a) + 1 + strlen(b) > MAX_PATH` is flawed because a string of length `MAX_PATH` requires `MAX_PATH + 1` bytes of storage (for the null terminator).
**Prevention:** When verifying buffer boundaries for string concatenation, always use `>=` against the maximum capacity of the buffer minus one, or ensure the check accounts for the null terminator. Ensure control flow after a failure is safe (e.g. return an error rather than infinitely looping).
