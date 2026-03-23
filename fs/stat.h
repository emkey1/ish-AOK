#ifndef FS_STAT_H
#define FS_STAT_H

#include "misc.h"

struct statbuf {
    qword_t dev;
    qword_t inode;
    dword_t mode;
    dword_t nlink;
    dword_t uid;
    dword_t gid;
    qword_t rdev;
    qword_t size;
    dword_t blksize;
    qword_t blocks;
    dword_t atime;
    dword_t atime_nsec;
    dword_t mtime;
    dword_t mtime_nsec;
    dword_t ctime;
    dword_t ctime_nsec;
};

struct oldstat {
    word_t dev;
    word_t ino;
    word_t mode;
    word_t nlink;
    word_t uid;
    word_t gid;
    word_t rdev;
    uint_t size;
    uint_t atime;
    uint_t mtime;
    uint_t ctime;
};

struct newstat {
    dword_t dev;
    dword_t ino;
    word_t mode;
    word_t nlink;
    word_t uid;
    word_t gid;
    dword_t rdev;
    dword_t size;
    dword_t blksize;
    dword_t blocks;
    dword_t atime;
    dword_t atime_nsec;
    dword_t mtime;
    dword_t mtime_nsec;
    dword_t ctime;
    dword_t ctime_nsec;
    char pad[8];
};

struct newstat64 {
    qword_t dev;
    char pad0[4];
    dword_t __st_ino;
    dword_t mode;
    dword_t nlink;
    dword_t uid;
    dword_t gid;
    qword_t rdev;
    char pad3[4];
    sqword_t size;
    dword_t blksize;
    qword_t blocks;
    dword_t atime;
    dword_t atime_nsec;
    dword_t mtime;
    dword_t mtime_nsec;
    dword_t ctime;
    dword_t ctime_nsec;
    qword_t ino;
} __attribute__((packed));

struct statx_timestamp_ {
    sqword_t tv_sec;
    dword_t tv_nsec;
    sdword_t __reserved;
} __attribute__((packed));

struct statx_ {
    dword_t stx_mask;
    dword_t stx_blksize;
    qword_t stx_attributes;
    dword_t stx_nlink;
    dword_t stx_uid;
    dword_t stx_gid;
    word_t stx_mode;
    word_t __spare0;
    qword_t stx_ino;
    qword_t stx_size;
    qword_t stx_blocks;
    qword_t stx_attributes_mask;
    struct statx_timestamp_ stx_atime;
    struct statx_timestamp_ stx_btime;
    struct statx_timestamp_ stx_ctime;
    struct statx_timestamp_ stx_mtime;
    dword_t stx_rdev_major;
    dword_t stx_rdev_minor;
    dword_t stx_dev_major;
    dword_t stx_dev_minor;
    qword_t stx_mnt_id;
    dword_t stx_dio_mem_align;
    dword_t stx_dio_offset_align;
    qword_t __spare3[12];
} __attribute__((packed));

struct statfsbuf {
    long type;
    long bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    uint64_t fsid;
    long namelen;
    long frsize;
    long flags;
    long spare[4];
};

struct statfs_ {
    uint_t type;
    uint_t bsize;
    uint_t blocks;
    uint_t bfree;
    uint_t bavail;
    uint_t files;
    uint_t ffree;
    uint64_t fsid;
    uint_t namelen;
    uint_t frsize;
    uint_t flags;
    uint_t spare[4];
} __attribute__((packed));

struct statfs64_ {
    uint_t type;
    uint_t bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    uint64_t fsid;
    uint_t namelen;
    uint_t frsize;
    uint_t flags;
    uint_t pad[4];
} __attribute__((packed));

#endif
