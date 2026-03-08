## 2024-05-24 - Kernel Stack Limitations
**Learning:** Allocating large buffers (e.g., 4096 bytes) directly on the kernel stack in `kernel/fs.c` (like inside `sys_read`) will cause kernel stack overflow. In this architecture, the kernel stack is small and bounded.
**Action:** When optimizing away `malloc` calls for small operations in the kernel, limit stack-allocated buffers to small, safe sizes like 256 bytes and fall back to heap allocation for larger requests.

## 2024-05-25 - sys_readv and sys_writev performance
**Learning:** System calls `sys_readv` and `sys_writev` in `kernel/fs.c` flatten vectorized I/O requests into a single buffer. Before, they always used `malloc()` to allocate this buffer, regardless of the size. This incurs significant performance overhead for small readv/writev calls which are very common.
**Action:** Implemented a fast path using an explicitly aligned `256`-byte stack buffer for small `sys_readv` and `sys_writev` requests, similar to existing optimizations in `sys_read`, `sys_write`, `sys_pread` and `sys_pwrite`. Only requests larger than 256 bytes will now fall back to heap allocation. Added `__attribute__((aligned(16)))` to stack buffers for consistency.
