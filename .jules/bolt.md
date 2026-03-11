## 2024-05-24 - Kernel Stack Limitations
**Learning:** Allocating large buffers (e.g., 4096 bytes) directly on the kernel stack in `kernel/fs.c` (like inside `sys_read`) will cause kernel stack overflow. In this architecture, the kernel stack is small and bounded.
**Action:** When optimizing away `malloc` calls for small operations in the kernel, limit stack-allocated buffers to small, safe sizes like 256 bytes and fall back to heap allocation for larger requests.

## 2024-05-25 - Caching String Lengths for Fast Path Evaluation
**Learning:** `strlen` calls inside hot loops for checking paths (e.g. in `mount_find` or list sorting) result in an O(N^2) evaluation over multiple iterations since path component sizes don't change.
**Action:** Always pre-calculate `strlen` on structures (like `mount->point_len`) and use the cached value inside of hot loops to avoid repeated evaluation.
