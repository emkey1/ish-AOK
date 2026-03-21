## 2024-05-24 - Kernel Stack Limitations
**Learning:** Allocating large buffers (e.g., 4096 bytes) directly on the kernel stack in `kernel/fs.c` (like inside `sys_read`) will cause kernel stack overflow. In this architecture, the kernel stack is small and bounded.
**Action:** When optimizing away `malloc` calls for small operations in the kernel, limit stack-allocated buffers to small, safe sizes like 256 bytes and fall back to heap allocation for larger requests.

## 2024-05-25 - sys_readv and sys_writev performance
**Learning:** System calls `sys_readv` and `sys_writev` in `kernel/fs.c` flatten vectorized I/O requests into a single buffer. Before, they always used `malloc()` to allocate this buffer, regardless of the size. This incurs significant performance overhead for small readv/writev calls which are very common.
**Action:** Implemented a fast path using an explicitly aligned `256`-byte stack buffer for small `sys_readv` and `sys_writev` requests, similar to existing optimizations in `sys_read`, `sys_write`, `sys_pread` and `sys_pwrite`. Only requests larger than 256 bytes will now fall back to heap allocation. Added `__attribute__((aligned(16)))` to stack buffers for consistency.

## 2024-05-26 - Redundant string operations in VFS loops
**Learning:** Functions like `strlen()` are frequently re-evaluated inside VFS list traversals (e.g., `list_for_each_entry` in `fs/mount.c` and `fs/generic.c`), causing unnecessary O(N) recalculations for loop-invariant variables. Some structures like `struct mount` already cache string lengths (e.g., `point_len`), but these are sometimes ignored in favor of re-running `strlen()`.
**Action:** When working on VFS path matching or list traversals, hoist loop-invariant `strlen()` calls outside loops, and always utilize pre-cached lengths in structs (like `mount->point_len`) to prevent performance degradation during deep traversals.

## 2026-03-18 - Optimize string concatenation in fakefs_readdir
**Learning:** In performance-critical VFS loops like fakefs_readdir, using strcat causes O(N) recalculations of string length. Since the lengths of the directory path and entry name are already known or computed for bounds checking, they can be reused to perform direct array assignment and memcpy.
**Action:** Replace O(N) strcat calls with direct array assignment (e.g., path[len] = '/') and memcpy() using pre-calculated string lengths to prevent performance degradation during directory traversal.

## 2026-03-22 - Optimize single-character NSString creation
**Learning:** In `app/TerminalView.m`, the `addKeys:withModifiers:` method is a performance bottleneck for input processing. The method historically used `[NSString stringWithFormat:@"%c", keys[i]]` inside a loop, which is computationally expensive because it involves format string parsing and dynamic evaluation for every character.
**Action:** Replace the expensive `stringWithFormat:` call with `[[NSString alloc] initWithBytes:&keys[i] length:1 encoding:NSUTF8StringEncoding]` (with a fallback to `stringWithFormat:` for invalid UTF-8 sequences) for direct byte-to-string mapping, significantly speeding up key binding initialization during the application's startup and layout phases.
## 2026-03-23 - Avoid strcat in __path_normalize
**Learning:** In `__path_normalize` (`fs/path.c`), using `strcat` when computing symlink resolutions causes O(N) calculations, and calling `strlen` repeatedly on unchanged source strings incurs unnecessary overhead.
**Action:** Replace `strcpy` and `strcat` with explicit length calculations and `memcpy`. Hoist `strlen` calls outside of paths that evaluate the same variable multiple times.
## 2026-03-24 - Optimize dynamic string construction
**Learning:** In `fs/proc/ish.c`, the `parse_if_flags` function used `strcat` to append strings dynamically, which causes O(N) traversal to find the end of the string for every append.
**Action:** Replace `strcat` in dynamic string construction with `memcpy` using pre-calculated string lengths. By keeping track of the current string length `len`, appending becomes an O(1) operation.
