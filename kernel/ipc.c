#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/mm.h"
#include "kernel/task.h"
#include "emu/memory.h"
#include "util/list.h"
#include "util/sync.h"

#define IPCOP_SEMOP_   1
#define IPCOP_SEMGET_  2
#define IPCOP_SEMCTL_  3
#define IPCOP_SEMTIMEDOP_ 4
#define IPCOP_MSGSND_  11
#define IPCOP_MSGRCV_  12
#define IPCOP_MSGGET_  13
#define IPCOP_MSGCTL_  14
#define IPCOP_SHMAT_   21
#define IPCOP_SHMDT_   22
#define IPCOP_SHMGET_  23
#define IPCOP_SHMCTL_  24

#define IPC_PRIVATE_ 0
#define IPC_CREAT_   01000
#define IPC_EXCL_    02000

#define IPC_RMID_    0
#define IPC_SET_     1
#define IPC_STAT_    2
#define IPC_64_      0x100

#define SHM_RDONLY_  010000
#define SHM_RND_     020000

#include "kernel/sysvipc.h"

bool ipc_access_ok(uid_t_ uid, uid_t_ gid, uid_t_ cuid, uid_t_ cgid,
                   mode_t_ mode, int want) {
    if (current == NULL)
        return false;
    if (current_capable(CAP_SYS_ADMIN_))
        return true;
    // Owner bits if we own it, else group bits if we are in its group, else
    // other bits -- and the chosen class is final, exactly as on Linux. A
    // 0600 object owned by someone else is refused even though the "other"
    // bits would be consulted for a stranger, because the group class already
    // matched and lost.
    mode_t_ granted;
    if (current->euid == uid || current->euid == cuid)
        granted = (mode >> 6) & 7;
    else if (current->egid == gid || current->egid == cgid)
        granted = (mode >> 3) & 7;
    else
        granted = mode & 7;
    return (granted & (mode_t_) want) == (mode_t_) want;
}

bool ipc_owner_ok(uid_t_ uid, uid_t_ cuid) {
    if (current == NULL)
        return false;
    return current_capable(CAP_SYS_ADMIN_) ||
           current->euid == uid || current->euid == cuid;
}

struct kernel_shmid64_ds_i386_ {
    struct ipc_perm_i386_ shm_perm;
    dword_t shm_segsz;
    dword_t shm_atime;
    dword_t shm_atime_high;
    dword_t shm_dtime;
    dword_t shm_dtime_high;
    dword_t shm_ctime;
    dword_t shm_ctime_high;
    pid_t_ shm_cpid;
    pid_t_ shm_lpid;
    dword_t shm_nattch;
    dword_t __unused4;
    dword_t __unused5;
};
static_assert(sizeof(struct kernel_shmid64_ds_i386_) == 84, "i386 kernel shmid64_ds size");

struct shmid_ds_amd64_ {
    struct ipc_perm_amd64_ shm_perm;
    qword_t shm_segsz;
    sqword_t shm_atime;
    sqword_t shm_dtime;
    sqword_t shm_ctime;
    pid_t_ shm_cpid;
    pid_t_ shm_lpid;
    qword_t shm_nattch;
    qword_t __unused4;
    qword_t __unused5;
};
static_assert(sizeof(struct shmid_ds_amd64_) == 112, "amd64 shmid_ds size");

struct shm_segment {
    struct list shm_segments;
    dword_t key;
    int id;
    size_t size;
    size_t alloc_size;
    pages_t pages;
    int fd;
    mode_t_ mode;
    uid_t_ uid;
    uid_t_ gid;
    uid_t_ cuid;
    uid_t_ cgid;
    pid_t_ cpid;
    pid_t_ lpid;
    time_t_ atime;
    time_t_ dtime;
    time_t_ ctime;
    unsigned nattch;
    bool removed;
};

struct shm_region {
    struct list mm_regions;
    struct shm_segment *segment;
    guest_addr_t addr;
    pages_t pages;
};

static struct list shm_segments = LIST_INITIALIZER(shm_segments);
static lock_t shm_lock = LOCK_INITIALIZER;
static int shm_next_id = 1;

static bool ipc_trace_enabled(void) {
    return false;
}

static time_t_ ipc_now(void) {
    return (time_t_) sys_time(0);
}

static void shm_segment_maybe_destroy(struct shm_segment *segment) {
    if (segment == NULL || !segment->removed || segment->nattch != 0)
        return;
    list_remove(&segment->shm_segments);
    close(segment->fd);
    free(segment);
}

static struct shm_segment *shm_segment_find_by_key(dword_t key) {
    struct shm_segment *segment;
    list_for_each_entry(&shm_segments, segment, shm_segments) {
        if (!segment->removed && segment->key == key)
            return segment;
    }
    return NULL;
}

static struct shm_segment *shm_segment_find_by_id(int id) {
    struct shm_segment *segment;
    list_for_each_entry(&shm_segments, segment, shm_segments) {
        if (segment->id == id)
            return segment;
    }
    return NULL;
}

static int shm_backing_create(size_t size) {
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0')
        tmpdir = "/tmp";
    bool has_trailing_slash = tmpdir[strlen(tmpdir) - 1] == '/';
    size_t path_len = strlen(tmpdir) + sizeof("ish-shm.XXXXXX") + (has_trailing_slash ? 0 : 1);
    char *path = malloc(path_len);
    if (path == NULL)
        return -1;
    snprintf(path, path_len, has_trailing_slash ? "%sish-shm.XXXXXX" : "%s/ish-shm.XXXXXX", tmpdir);
    int fd = mkstemp(path);
    if (fd < 0)
        goto out;
    unlink(path);
    if (ftruncate(fd, (off_t) size) < 0) {
        close(fd);
        fd = -1;
    }
out:
    free(path);
    if (fd < 0)
        return -1;
    return fd;
}

static int shmget_internal(dword_t key, size_t size, int flags) {
    lock(&shm_lock, 0);
    struct shm_segment *segment = key == IPC_PRIVATE_ ? NULL : shm_segment_find_by_key(key);
    if (segment != NULL) {
        if ((flags & IPC_CREAT_) && (flags & IPC_EXCL_)) {
            unlock(&shm_lock);
            return _EEXIST;
        }
        if (size != 0 && size > segment->size) {
            unlock(&shm_lock);
            return _EINVAL;
        }
        int id = segment->id;
        unlock(&shm_lock);
        return id;
    }
    if (!(flags & IPC_CREAT_) && key != IPC_PRIVATE_) {
        unlock(&shm_lock);
        return _ENOENT;
    }
    if (size == 0) {
        unlock(&shm_lock);
        return _EINVAL;
    }

    pages_t pages = PAGE_ROUND_UP(size);
    size_t rounded = pages * PAGE_SIZE;
    int fd = shm_backing_create(rounded);
    if (fd < 0) {
        int err = errno_map();
        unlock(&shm_lock);
        return err;
    }

    segment = calloc(1, sizeof(*segment));
    if (segment == NULL) {
        close(fd);
        unlock(&shm_lock);
        return _ENOMEM;
    }
    *segment = (struct shm_segment) {
        .key = key,
        .id = shm_next_id++,
        .size = size,
        .alloc_size = rounded,
        .pages = pages,
        .fd = fd,
        .mode = flags & 0777,
        .uid = current->euid,
        .gid = current->egid,
        .cuid = current->euid,
        .cgid = current->egid,
        .cpid = current->pid,
        .lpid = current->pid,
        .ctime = ipc_now(),
    };
    list_init(&segment->shm_segments);
    list_add_tail(&shm_segments, &segment->shm_segments);
    int id = segment->id;
    unlock(&shm_lock);
    return id;
}

static guest_addr_t shm_region_attach(struct mm *mm, struct shm_segment *segment, guest_addr_t attach_addr, int shmflg) {
    int prot = PROT_READ;
    unsigned mem_flags = P_READ | P_SHARED;
    if (!(shmflg & SHM_RDONLY_)) {
        prot |= PROT_WRITE;
        mem_flags |= P_WRITE;
    }

    void *mapping = mmap(NULL, segment->alloc_size, prot, MAP_SHARED, segment->fd, 0);
    if (mapping == MAP_FAILED)
        return (guest_addr_t) errno_map();

    page_t page = 0;
    if (attach_addr != 0) {
        if (shmflg & SHM_RND_)
            attach_addr = BYTES_ROUND_DOWN(attach_addr);
        else if (PGOFFSET(attach_addr) != 0) {
            munmap(mapping, segment->alloc_size);
            return (guest_addr_t) _EINVAL;
        }
        page = PAGE(attach_addr);
    }

    write_lock(&mm->mem.lock);
    if (attach_addr != 0) {
        page = PAGE(attach_addr);
        if (!pt_is_hole(&mm->mem, page, segment->pages)) {
            write_unlock(&mm->mem.lock);
            munmap(mapping, segment->alloc_size);
            return (guest_addr_t) _EINVAL;
        }
    } else {
        page = pt_find_hole(&mm->mem, segment->pages);
        if (page == BAD_PAGE) {
            write_unlock(&mm->mem.lock);
            munmap(mapping, segment->alloc_size);
            return (guest_addr_t) _ENOMEM;
        }
        attach_addr = (guest_addr_t) page << PAGE_BITS;
    }

    int err = pt_map(&mm->mem, page, segment->pages, mapping, 0, mem_flags);
    if (err < 0) {
        write_unlock(&mm->mem.lock);
        munmap(mapping, segment->alloc_size);
        return (guest_addr_t) err;
    }
    struct pt_entry *entry = mem_pt(&mm->mem, page);
    if (entry != NULL && entry->data != NULL)
        entry->data->shared_key = (uintptr_t) segment;

    struct shm_region *region = malloc(sizeof(*region));
    if (region == NULL) {
        pt_unmap_always(&mm->mem, page, segment->pages);
        write_unlock(&mm->mem.lock);
        return (guest_addr_t) _ENOMEM;
    }
    *region = (struct shm_region) {
        .segment = segment,
        .addr = attach_addr,
        .pages = segment->pages,
    };
    list_init(&region->mm_regions);
    list_add_tail(&mm->shm_regions, &region->mm_regions);
    write_unlock(&mm->mem.lock);

    lock(&shm_lock, 0);
    segment->nattch++;
    segment->lpid = current->pid;
    segment->atime = ipc_now();
    unlock(&shm_lock);

    return attach_addr;
}

static guest_addr_t shmat_internal(int id, guest_addr_t shmaddr, int shmflg) {
    lock(&shm_lock, 0);
    struct shm_segment *segment = shm_segment_find_by_id(id);
    unlock(&shm_lock);
    if (segment == NULL || segment->removed)
        return (guest_addr_t) _EINVAL;
    // Attaching needs read, and write too unless SHM_RDONLY was asked for.
    // Without this any uid attached to and wrote a root-owned 0600 segment.
    int want = 4;
    if (!(shmflg & SHM_RDONLY_))
        want |= 2;
    if (!ipc_access_ok(segment->uid, segment->gid, segment->cuid, segment->cgid,
                       segment->mode, want))
        return (guest_addr_t) _EACCES;
    return shm_region_attach(current->mm, segment, shmaddr, shmflg);
}

static int shmdt_internal(struct mm *mm, guest_addr_t addr, pid_t_ lpid, bool from_release) {
    write_lock(&mm->mem.lock);
    struct shm_region *region, *tmp;
    list_for_each_entry_safe(&mm->shm_regions, region, tmp, mm_regions) {
        if (region->addr != addr)
            continue;
        pt_unmap_always(&mm->mem, PAGE(region->addr), region->pages);
        list_remove(&region->mm_regions);
        struct shm_segment *segment = region->segment;
        free(region);
        write_unlock(&mm->mem.lock);

        lock(&shm_lock, 0);
        if (segment->nattch != 0)
            segment->nattch--;
        segment->lpid = lpid;
        segment->dtime = ipc_now();
        shm_segment_maybe_destroy(segment);
        unlock(&shm_lock);
        return 0;
    }
    write_unlock(&mm->mem.lock);
    return from_release ? 0 : _EINVAL;
}

static int shmctl_ipc_set(struct shm_segment *segment, uid_t_ uid, uid_t_ gid, mode_t_ mode) {
    segment->uid = uid;
    segment->gid = gid;
    segment->mode = mode & 0777;
    segment->ctime = ipc_now();
    return 0;
}

static void shmctl_fill_ipc_perm_i386(struct ipc_perm_i386_ *perm, struct shm_segment *segment) {
    *perm = (struct ipc_perm_i386_) {
        .key = segment->key,
        .uid = segment->uid,
        .gid = segment->gid,
        .cuid = segment->cuid,
        .cgid = segment->cgid,
        .mode = segment->mode,
        .seq = 0,
    };
}

static void shmctl_fill_ipc_perm_amd64(struct ipc_perm_amd64_ *perm, struct shm_segment *segment) {
    *perm = (struct ipc_perm_amd64_) {
        .key = segment->key,
        .uid = segment->uid,
        .gid = segment->gid,
        .cuid = segment->cuid,
        .cgid = segment->cgid,
        .mode = segment->mode,
        .seq = 0,
    };
}

static int shmctl_internal_i386(int id, int cmd, addr_t ptr) {
    int cmd_base = cmd & ~IPC_64_;
    lock(&shm_lock, 0);
    struct shm_segment *segment = shm_segment_find_by_id(id);
    if (segment == NULL) {
        unlock(&shm_lock);
        return _EINVAL;
    }

    if (cmd_base == IPC_RMID_) {
        // Destroying is the owner's or creator's privilege, not everyone's.
        if (!ipc_owner_ok(segment->uid, segment->cuid)) {
            unlock(&shm_lock);
            return _EPERM;
        }
        segment->removed = true;
        shm_segment_maybe_destroy(segment);
        unlock(&shm_lock);
        return 0;
    }

    if (cmd_base == IPC_SET_) {
        // Changing ownership or mode: owner or creator only.
        if (!ipc_owner_ok(segment->uid, segment->cuid)) {
            unlock(&shm_lock);
            return _EPERM;
        }
        struct kernel_shmid64_ds_i386_ info;
        unlock(&shm_lock);
        if (ptr == 0)
            return _EFAULT;
        if (user_read(ptr, &info, sizeof(info)))
            return _EFAULT;
        lock(&shm_lock, 0);
        segment = shm_segment_find_by_id(id);
        if (segment == NULL) {
            unlock(&shm_lock);
            return _EINVAL;
        }
        shmctl_ipc_set(segment, info.shm_perm.uid, info.shm_perm.gid, (mode_t_) info.shm_perm.mode);
        unlock(&shm_lock);
        return 0;
    }

    if (cmd_base == IPC_STAT_) {
        struct kernel_shmid64_ds_i386_ info = {
            .shm_segsz = segment->size,
            .shm_atime = segment->atime,
            .shm_dtime = segment->dtime,
            .shm_ctime = segment->ctime,
            .shm_cpid = segment->cpid,
            .shm_lpid = segment->lpid,
            .shm_nattch = segment->nattch,
        };
        shmctl_fill_ipc_perm_i386(&info.shm_perm, segment);
        unlock(&shm_lock);
        if (ptr == 0)
            return _EFAULT;
        if (user_write(ptr, &info, sizeof(info)))
            return _EFAULT;
        return 0;
    }

    unlock(&shm_lock);
    return _EINVAL;
}

static int shmctl_internal_amd64(int id, int cmd, guest_addr_t ptr) {
    int cmd_base = cmd & ~IPC_64_;
    lock(&shm_lock, 0);
    struct shm_segment *segment = shm_segment_find_by_id(id);
    if (segment == NULL) {
        unlock(&shm_lock);
        return _EINVAL;
    }

    if (cmd_base == IPC_RMID_) {
        // Destroying is the owner's or creator's privilege, not everyone's.
        if (!ipc_owner_ok(segment->uid, segment->cuid)) {
            unlock(&shm_lock);
            return _EPERM;
        }
        segment->removed = true;
        shm_segment_maybe_destroy(segment);
        unlock(&shm_lock);
        return 0;
    }

    if (cmd_base == IPC_SET_) {
        // Changing ownership or mode: owner or creator only.
        if (!ipc_owner_ok(segment->uid, segment->cuid)) {
            unlock(&shm_lock);
            return _EPERM;
        }
        struct shmid_ds_amd64_ info;
        unlock(&shm_lock);
        if (ptr == 0)
            return _EFAULT;
        if (user_read(ptr, &info, sizeof(info)))
            return _EFAULT;
        lock(&shm_lock, 0);
        segment = shm_segment_find_by_id(id);
        if (segment == NULL) {
            unlock(&shm_lock);
            return _EINVAL;
        }
        shmctl_ipc_set(segment, info.shm_perm.uid, info.shm_perm.gid, info.shm_perm.mode);
        unlock(&shm_lock);
        return 0;
    }

    if (cmd_base == IPC_STAT_) {
        struct shmid_ds_amd64_ info = {
            .shm_segsz = segment->size,
            .shm_atime = segment->atime,
            .shm_dtime = segment->dtime,
            .shm_ctime = segment->ctime,
            .shm_cpid = segment->cpid,
            .shm_lpid = segment->lpid,
            .shm_nattch = segment->nattch,
        };
        shmctl_fill_ipc_perm_amd64(&info.shm_perm, segment);
        unlock(&shm_lock);
        if (ptr == 0)
            return _EFAULT;
        if (user_write(ptr, &info, sizeof(info)))
            return _EFAULT;
        return 0;
    }

    unlock(&shm_lock);
    return _EINVAL;
}

void ipc_mm_init(struct mm *mm) {
    list_init(&mm->shm_regions);
}

void ipc_mm_copy(struct mm *dst, struct mm *src) {
    struct shm_region *region;
    list_for_each_entry(&src->shm_regions, region, mm_regions) {
        struct shm_region *copy = malloc(sizeof(*copy));
        if (copy == NULL)
            continue;
        *copy = (struct shm_region) {
            .segment = region->segment,
            .addr = region->addr,
            .pages = region->pages,
        };
        list_init(&copy->mm_regions);
        list_add_tail(&dst->shm_regions, &copy->mm_regions);

        lock(&shm_lock, 0);
        copy->segment->nattch++;
        unlock(&shm_lock);
    }
}

void ipc_mm_release(struct mm *mm) {
    struct shm_region *region, *tmp;
    list_for_each_entry_safe(&mm->shm_regions, region, tmp, mm_regions) {
        guest_addr_t addr = region->addr;
        shmdt_internal(mm, addr, current ? current->pid : 0, true);
    }
}

static int_t sys_ipc_common(uint_t call, int_t first, int_t second, guest_addr_t third,
        guest_addr_t ptr, int_t fifth) {
    STRACE("ipc(%u, %d, %d, %#llx, %#llx, %d)", call, first, second,
            (unsigned long long) third, (unsigned long long) ptr, fifth);
    use(fifth);

    uint_t version = call >> 16;
    uint_t op = call & 0xffff;
    use(version);
    if (ipc_trace_enabled()) {
        printk("ipc trace: pid=%d comm=%s call=%#x version=%u op=%u first=%d second=%d third=%#llx ptr=%#llx fifth=%d\n",
               current->pid, current->comm, call, version, op, first, second,
               (unsigned long long) third, (unsigned long long) ptr, fifth);
    }

    switch (op) {
        case IPCOP_SEMGET_:
            return sys_semget_guest((dword_t) first, second, (int_t) third);
        case IPCOP_SEMOP_:
            return sys_semop_guest(first, ptr, (uint_t) second);
        case IPCOP_SEMTIMEDOP_:
            // Through uint_t first: `fifth` is int_t (signed 32-bit) but for
            // SEMTIMEDOP it carries the TIMESPEC POINTER, and a 32-bit guest
            // address with bit 31 set sign-extended into 0xffffffff_xxxxxxxx,
            // which user_get then refused. semtimedop returned EFAULT for any
            // timeout living in the upper 2 GiB of an i386 address space.
            return sys_semtimedop_guest(first, ptr, (uint_t) second,
                                        (guest_addr_t) (uint_t) fifth);
        case IPCOP_SEMCTL_: {
            // The semun union is passed indirectly: ptr points at it.
            addr_t arg;
            if (user_read(ptr, &arg, sizeof(arg)))
                return _EFAULT;
            return sys_semctl_guest(first, second, (int_t) third, arg);
        }
        case IPCOP_MSGGET_:
            return sys_msgget_guest((dword_t) first, second);
        case IPCOP_MSGSND_:
            return sys_msgsnd_guest(first, ptr, (qword_t) (dword_t) second, (int_t) third);
        case IPCOP_MSGRCV_: {
            if (version == 1)
                return sys_msgrcv_guest(first, ptr, (qword_t) (dword_t) second,
                                        (sqword_t) fifth, (int_t) third);
            // Old-style: ptr points at struct ipc_kludge { msgp; msgtyp; }.
            struct { addr_t msgp; sdword_t msgtyp; } kludge;
            if (user_read(ptr, &kludge, sizeof(kludge)))
                return _EFAULT;
            return sys_msgrcv_guest(first, kludge.msgp, (qword_t) (dword_t) second,
                                    (sqword_t) kludge.msgtyp, (int_t) third);
        }
        case IPCOP_MSGCTL_:
            return sys_msgctl_guest(first, second, ptr);
        case IPCOP_SHMGET_:
            return shmget_internal((dword_t) first, (size_t) second, (int_t) third);
        case IPCOP_SHMAT_: {
            guest_addr_t out_addr = shmat_internal(first, ptr, second);
            if (ipc_trace_enabled()) {
                printk("ipc trace: pid=%d comm=%s shmat via ipc shmid=%d shmaddr=%#llx shmflg=%#x result=%#llx\n",
                       current->pid, current->comm, first, (unsigned long long) ptr,
                       second, (unsigned long long) out_addr);
            }
            if (IS_ERR((void *) (uintptr_t) out_addr))
                return (int_t) PTR_ERR((void *) (uintptr_t) out_addr);
            if (current->abi == GUEST_ABI_AMD64) {
                if (user_put((guest_addr_t) third, out_addr))
                    return _EFAULT;
            } else {
                addr_t out_addr_i386 = (addr_t) out_addr;
                if (user_put((guest_addr_t) third, out_addr_i386))
                    return _EFAULT;
            }
            return 0;
        }
        case IPCOP_SHMDT_:
            return shmdt_internal(current->mm, ptr, current->pid, false);
        case IPCOP_SHMCTL_:
            if (current->abi == GUEST_ABI_AMD64)
                return shmctl_internal_amd64(first, second, ptr);
            return shmctl_internal_i386(first, second, (addr_t) ptr);
        default:
            if (ipc_trace_enabled()) {
                printk("ipc trace: pid=%d comm=%s unimplemented ipc op=%u version=%u\n",
                       current->pid, current->comm, op, version);
            }
            return _ENOSYS;
    }
}

int_t sys_ipc(uint_t call, int_t first, int_t second, addr_t third, addr_t ptr, int_t fifth) {
    return sys_ipc_common(call, first, second, third, ptr, fifth);
}

int_t sys_ipc_guest(uint_t call, int_t first, int_t second, guest_addr_t third, guest_addr_t ptr, int_t fifth) {
    return sys_ipc_common(call, first, second, third, ptr, fifth);
}

int_t sys_shmget(dword_t key, dword_t size, dword_t shmflg) {
    return shmget_internal(key, size, shmflg);
}

int_t sys_shmget_guest(dword_t key, qword_t size, dword_t shmflg) {
    return shmget_internal(key, (size_t) size, shmflg);
}

addr_t sys_shmat(int_t shmid, addr_t shmaddr, int_t shmflg) {
    guest_addr_t result = shmat_internal(shmid, shmaddr, shmflg);
    if (ipc_trace_enabled()) {
        printk("ipc trace: pid=%d comm=%s shmat direct shmid=%d shmaddr=%#x shmflg=%#x result=%#llx\n",
               current->pid, current->comm, shmid, shmaddr, shmflg,
               (unsigned long long) result);
    }
    return (addr_t) result;
}

guest_addr_t sys_shmat_guest(int_t shmid, guest_addr_t shmaddr, int_t shmflg) {
    return shmat_internal(shmid, shmaddr, shmflg);
}

int_t sys_shmdt(addr_t shmaddr) {
    return shmdt_internal(current->mm, shmaddr, current->pid, false);
}

int_t sys_shmdt_guest(guest_addr_t shmaddr) {
    return shmdt_internal(current->mm, shmaddr, current->pid, false);
}

int_t sys_shmctl(int_t shmid, int_t cmd, addr_t buf) {
    return shmctl_internal_i386(shmid, cmd, buf);
}

int_t sys_shmctl_guest(int_t shmid, int_t cmd, guest_addr_t buf) {
    return shmctl_internal_amd64(shmid, cmd, buf);
}
