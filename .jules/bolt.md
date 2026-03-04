## 2024-05-24 - Kernel Stack Limitations
**Learning:** Allocating large buffers (e.g., 4096 bytes) directly on the kernel stack in `kernel/fs.c` (like inside `sys_read`) will cause kernel stack overflow. In this architecture, the kernel stack is small and bounded.
**Action:** When optimizing away `malloc` calls for small operations in the kernel, limit stack-allocated buffers to small, safe sizes like 256 bytes and fall back to heap allocation for larger requests.

## 2024-10-18 - User Memory Access Pattern Optimizations
**Learning:** Functions like `user_read_string` and `user_write_string` in `kernel/user.c` used a character-by-character read/write loop. In each iteration, they would call `__user_read_task` or `__user_write_task`, which recalculates the page boundaries and looks up memory pointers on every single byte. This caused excessive overhead (around 20x to 270x slower for large strings).
**Action:** Always process user memory access (`user_read`/`user_write`) in page-aligned bulk chunks (up to the next page boundary) instead of byte-by-byte loops, reusing the calculated pointers from `mem_ptr`.
