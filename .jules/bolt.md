## 2024-05-24 - Kernel Stack Limitations
**Learning:** Allocating large buffers (e.g., 4096 bytes) directly on the kernel stack in `kernel/fs.c` (like inside `sys_read`) will cause kernel stack overflow. In this architecture, the kernel stack is small and bounded.
**Action:** When optimizing away `malloc` calls for small operations in the kernel, limit stack-allocated buffers to small, safe sizes like 256 bytes and fall back to heap allocation for larger requests.

## 2024-05-25 - Caching String Lengths for Fast Path Resolution
**Learning:** Hot paths like `mount_find` in `fs/mount.c` recalculate string lengths in loops (`strlen(mount->point)`). This is O(N) over path length per mount checked and significantly slows down VFS operations.
**Action:** When iterating over lists of paths, strings, or mount points, prefer pre-calculating and caching their lengths. The length can be stored inside structs like `struct mount` (e.g. `point_len`) upon initialization, turning subsequent length checks into O(1) reads.
