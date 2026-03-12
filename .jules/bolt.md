## 2024-05-24 - Kernel Stack Limitations
**Learning:** Allocating large buffers (e.g., 4096 bytes) directly on the kernel stack in `kernel/fs.c` (like inside `sys_read`) will cause kernel stack overflow. In this architecture, the kernel stack is small and bounded.
**Action:** When optimizing away `malloc` calls for small operations in the kernel, limit stack-allocated buffers to small, safe sizes like 256 bytes and fall back to heap allocation for larger requests.

## 2024-05-25 - Redundant string lengths in loop
**Learning:** In VFS functions like `fs/generic.c`, calculating string lengths (e.g. `strlen(path)`) inside iteration loops (e.g. `list_for_each_entry`) degrades performance from O(N+M) to O(N*M) where N is loop iterations and M is path length.
**Action:** Pre-calculate `strlen()` outside loops to prevent O(N) performance degradation during list traversals.
