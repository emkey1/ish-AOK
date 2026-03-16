## 2026-02-23 - Buffer Overflow in Logging
**Vulnerability:** Unsafe use of `vsprintf` in `ish_vprintk` (16KB static buffer) and `die` (4KB stack buffer) allowed potential buffer overflows if log messages exceeded buffer size. The code explicitly noted "I'm trusting you to not pass an absurdly long message", highlighting known technical debt.
**Learning:** Even "trusted" internal callers (like `STRACE` via `printk`) can inadvertently trigger overflows with user-controlled input (e.g., long arguments to `execve`).
**Prevention:** Always use `vsnprintf` to enforce buffer limits. Implement graceful truncation or forced flushing when buffers fill up to prevent data loss or crashes.

## 2026-02-24 - Hidden Error Handling Bypass in Kernel Memory Access
**Vulnerability:** `user_read_string` contained a logic error where the return value of `__user_read_task` was discarded using the comma operator with `false` (`func(), false`), causing the error check to always fail (i.e., assume success).
**Learning:** The comma operator can be dangerous when used in conditional statements, especially if it looks like a function argument or a typo. It can silently suppress error checks.
**Prevention:** Always verify argument counts and parenthesis matching. Use static analysis tools that might catch "statement has no effect" or "comma operator used in if condition".

## 2024-05-23 - Buffer Overflow in boot_command_line assignment
**Vulnerability:**
The `boot_command_line` buffer could be overflowed in `app/LinuxInterop.c` via unbounded `strcpy` when executing `actuate_kernel(cmdline)`, and in `linux/main.c` via unbounded `strcat` when parsing arguments.
**Learning:**
Always use bounding functions (`strncpy` or `strncat`) when interacting with `boot_command_line` buffer or other static arrays to prevent buffer overflows that could lead to malicious code execution or kernel crash.
**Prevention:**
Enforce strict bounds checking. `strncpy(dest, src, sizeof(dest) - 1); dest[sizeof(dest) - 1] = '\0';` and `strncat(dest, src, sizeof(dest) - strlen(dest) - 1);` should be used instead of their unbounded counterparts.
