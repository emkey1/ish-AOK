## 2024-05-24 - Kernel Stack Limitations
**Learning:** Allocating large buffers (e.g., 4096 bytes) directly on the kernel stack in `kernel/fs.c` (like inside `sys_read`) will cause kernel stack overflow. In this architecture, the kernel stack is small and bounded.
**Action:** When optimizing away `malloc` calls for small operations in the kernel, limit stack-allocated buffers to small, safe sizes like 256 bytes and fall back to heap allocation for larger requests.

## 2026-03-15 - Optimize VFS string lengths
**Learning:** Struct-cached lengths and hoisting loop-invariant string traversals prevent O(N) performance degradation during VFS operations.
**Action:** Pre-calculate lengths outside loops or use struct-cached lengths (like `point_len` in `struct mount`) to avoid recalculating string lengths inside loops.
