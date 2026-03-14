## 2024-05-24 - Kernel Stack Limitations
**Learning:** Allocating large buffers (e.g., 4096 bytes) directly on the kernel stack in `kernel/fs.c` (like inside `sys_read`) will cause kernel stack overflow. In this architecture, the kernel stack is small and bounded.
**Action:** When optimizing away `malloc` calls for small operations in the kernel, limit stack-allocated buffers to small, safe sizes like 256 bytes and fall back to heap allocation for larger requests.

## 2024-05-25 - O(N) String Operations in VFS Hot Paths
**Learning:** `strlen` and `strcat` in critical VFS loops (like `fs/mount.c` parameter parsing and `fs/fake.c` directory reading) can cause severe performance degradation due to hidden O(N*M) or O(N^2) complexity. Additionally, prefix-matching loops like `mount_param_flag` using `strncmp(info, flag, flag_len)` without bound checks can accidentally match prefixes (e.g. `ro` matching `rootfs`). Using `strcspn(info, ",")` must correctly advance past the comma to avoid infinite loop bugs on trailing/multiple commas.
**Action:** Always hoist `strlen()` out of `while` and `for` loops if the string is immutable. In directory traversal loops like `fakefs_readdir`, replace `strcat()` with direct array indexing (e.g. `path[len] = '/'`) and `memcpy()` using pre-calculated and cached string lengths.
