## 2024-05-24 - Kernel Stack Limitations
**Learning:** Allocating large buffers (e.g., 4096 bytes) directly on the kernel stack in `kernel/fs.c` (like inside `sys_read`) will cause kernel stack overflow. In this architecture, the kernel stack is small and bounded.
**Action:** When optimizing away `malloc` calls for small operations in the kernel, limit stack-allocated buffers to small, safe sizes like 256 bytes and fall back to heap allocation for larger requests.

## 2024-05-27 - Unnecessary Allocation in sys_getcwd
**Learning:** The implementation of `sys_getcwd` allocated a heap buffer simply to copy the stack buffer `pwd` before writing it to user space. This is an unnecessary `malloc`/`free` cycle and a redundant string copy, since `user_write` can read directly from the stack buffer.
**Action:** Eliminate the heap allocation and copy by using `pwd` directly in the `user_write` call. This reduces memory pressure and execution time.
