## 2024-05-24 - Kernel Stack Limitations
**Learning:** Allocating large buffers (e.g., 4096 bytes) directly on the kernel stack in `kernel/fs.c` (like inside `sys_read`) will cause kernel stack overflow. In this architecture, the kernel stack is small and bounded.
**Action:** When optimizing away `malloc` calls for small operations in the kernel, limit stack-allocated buffers to small, safe sizes like 256 bytes and fall back to heap allocation for larger requests.

## 2025-05-15 - VFS Path Resolution Strings
**Learning:** Virtual file system path resolution algorithms frequently iterate through lists comparing strings. Calling `strlen()` on properties (like mount points) inside these loops causes performance degradation (O(N) operations inside a loop). `struct mount` already caches the string length in `point_len`.
**Action:** When performing string operations in loop traversals for path resolution, hoist loop-invariant string length calculations outside the loop, or use the pre-calculated, cached length properties defined on the structs (like `mount->point_len`) to eliminate redundant time complexity.
