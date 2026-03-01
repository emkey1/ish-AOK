## 2025-02-18 - [FakeFS Readdir Optimization]
**Learning:** `fakefs` directory listings were performing a SQLite transaction for *every single entry*, causing massive overhead. The filesystem abstraction lacked hooks to batch these operations.
**Action:** Implemented `readdir_begin` and `readdir_end` hooks in `fd_ops` and updated `fakefs` to use a single transaction for the entire directory listing. Also switched to recursive mutexes to safely handle nested transaction requests.

## 2025-02-18 - [Small I/O Buffer Optimization]
**Learning:** `malloc`/`free` has significant overhead when performed repeatedly for small I/O operations (like reading/writing to devices or processing small files) in `kernel/fs.c`.
**Action:** Replaced mandatory heap allocations with small 256-byte stack buffers that fall back to `malloc` only for larger inputs. This pattern drastically speeds up small read/write system calls.
