## 2025-02-18 - [FakeFS Readdir Optimization]
**Learning:** `fakefs` directory listings were performing a SQLite transaction for *every single entry*, causing massive overhead. The filesystem abstraction lacked hooks to batch these operations.
**Action:** Implemented `readdir_begin` and `readdir_end` hooks in `fd_ops` and updated `fakefs` to use a single transaction for the entire directory listing. Also switched to recursive mutexes to safely handle nested transaction requests.
