## 2024-05-24 - Kernel Stack Limitations
**Learning:** Allocating large buffers (e.g., 4096 bytes) directly on the kernel stack in `kernel/fs.c` (like inside `sys_read`) will cause kernel stack overflow. In this architecture, the kernel stack is small and bounded.
**Action:** When optimizing away `malloc` calls for small operations in the kernel, limit stack-allocated buffers to small, safe sizes like 256 bytes and fall back to heap allocation for larger requests.
## 2024-05-24 - [Optimize string concatenation in fakefs_readdir]
**Learning:** Using `strcat` in performance-critical VFS loops (like `fakefs_readdir` in `fs/fake.c`) introduces O(N) overhead as it repeatedly scans for the null terminator.
**Action:** Replace `strcat` calls with direct array assignment (`path[len] = '/'`) and `memcpy` using pre-calculated string lengths.
