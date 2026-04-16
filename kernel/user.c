#include <sys/uio.h>
#include <string.h>
#include "kernel/calls.h"

extern bool doEnableExtraLocking;
extern pthread_mutex_t extra_lock;

static int __user_read_task(struct task *task, addr_t addr, void *buf, size_t count) {
    char *cbuf = (char *) buf;
    addr_t p = addr;
    while (p < addr + count) {
        addr_t chunk_end = (PAGE(p) + 1) << PAGE_BITS;
        if (chunk_end > addr + count)
            chunk_end = addr + count;
  
        const char *ptr = mem_ptr(task->mem, p, MEM_READ);
        
        if (ptr == NULL) {
            return 1;
	}
        memcpy(&cbuf[p - addr], ptr, chunk_end - p);
        p = chunk_end;
    }
    return 0;
}

static int __user_write_task(struct task *task, addr_t addr, const void *buf, size_t count, bool ptrace) {
    const char *cbuf = (const char *) buf;
    addr_t p = addr;
    while (p < addr + count) {
        addr_t chunk_end = (PAGE(p) + 1) << PAGE_BITS;
        if (chunk_end > addr + count)
            chunk_end = addr + (addr_t)count;
        char *ptr = mem_ptr(task->mem, p, ptrace ? MEM_WRITE_PTRACE : MEM_WRITE);
        if (ptr == NULL)
            return 1;
        memcpy(ptr, &cbuf[p - addr], chunk_end - p);
     /*   if(!strcmp(task->comm, "ls")) {  // Turns out this code mostly deals with linked libraries, at least in the case of ls.  -mke
            char foo[500] = {};
            memcpy(foo, &cbuf[p - addr], 50);
            int a = 0;
            printk("INFO: FOO: %s\n", foo);
            memcpy(ptr, &cbuf[p - addr], chunk_end - p);
        } else {
            memcpy(ptr, &cbuf[p - addr], chunk_end - p);
        } */
        p = chunk_end;
    }
    return 0;
}

int user_read_task(struct task *task, addr_t addr, void *buf, size_t count) {
    read_lock(&task->mem->lock);
    int res = __user_read_task(task, addr, buf, count);
    read_unlock(&task->mem->lock);
    return res;
}

int user_read(addr_t addr, void *buf, size_t count) {
    return user_read_task(current, addr, buf, count);
}

int user_write_task(struct task *task, addr_t addr, const void *buf, size_t count) {
    read_lock(&task->mem->lock);
    task_ref_cnt_mod(task, 1);
    mem_ref_cnt_mod(task->mem, 1);
    int res = __user_write_task(task, addr, buf, count, false);
    read_unlock(&task->mem->lock);
    task_ref_cnt_mod(task, -1);
    mem_ref_cnt_mod(task->mem, -1);
    return res;
}

int user_write_task_ptrace(struct task *task, addr_t addr, const void *buf, size_t count) {
    read_lock(&task->mem->lock);
    task_ref_cnt_mod(task, 1);
    mem_ref_cnt_mod(task->mem, 1);
    int res = __user_write_task(task, addr, buf, count, true);
    read_unlock(&task->mem->lock);
    task_ref_cnt_mod(task, -1);
    mem_ref_cnt_mod(task->mem, -1);
    return res;
}

int user_write(addr_t addr, const void *buf, size_t count) {
    return user_write_task(current, addr, buf, count);
}

int user_read_string(addr_t addr, char *buf, size_t max) {
    if (addr == 0) {
        return 1;
    }
    read_lock(&current->mem->lock);
    size_t i = 0;
    while (i < max) {
        if (__user_read_task(current, addr + i, &buf[i], sizeof(buf[i]))) {
            read_unlock(&current->mem->lock);
            return 1;
        }
        if (buf[i] == '\0')
            break;
        i++;
    }
    read_unlock(&current->mem->lock);
    return 0;
}

int user_write_string(addr_t addr, const char *buf) {
    if (addr == 0) {
        return 1;
    }
    read_lock(&current->mem->lock);
    size_t i = 0;
    do {
        if (__user_write_task(current, addr + i, &buf[i], sizeof(buf[i]), false)) {
            read_unlock(&current->mem->lock);
            return 1;
        }
        i++;
    } while (buf[i - 1] != '\0');
    read_unlock(&current->mem->lock);
    return 0;
}

static struct iovec_ *user_read_iovecs(struct task *task, addr_t iov_addr, dword_t iov_count) {
    if (iov_count == 0)
        return NULL;
    if (iov_count > IOV_MAX)
        return ERR_PTR(_EINVAL);

    size_t size = sizeof(struct iovec_) * iov_count;
    struct iovec_ *iov = malloc(size);
    if (iov == NULL)
        return ERR_PTR(_ENOMEM);
    if (user_read_task(task, iov_addr, iov, size)) {
        free(iov);
        return ERR_PTR(_EFAULT);
    }
    return iov;
}

dword_t sys_process_vm_readv(pid_t_ pid, addr_t local_iov_addr, dword_t liovcnt,
                             addr_t remote_iov_addr, dword_t riovcnt, dword_t flags) {
    if (flags != 0)
        return _EINVAL;

    struct task *task = pid_get_task(pid);
    if (task == NULL)
        return _ESRCH;
    if (task != current && task->parent != current && current->parent != task)
        return _EPERM;

    struct iovec_ *local_iov = user_read_iovecs(current, local_iov_addr, liovcnt);
    if (IS_ERR(local_iov))
        return PTR_ERR(local_iov);
    struct iovec_ *remote_iov = user_read_iovecs(current, remote_iov_addr, riovcnt);
    if (IS_ERR(remote_iov)) {
        free(local_iov);
        return PTR_ERR(remote_iov);
    }

    dword_t local_index = 0, remote_index = 0;
    size_t local_off = 0, remote_off = 0;
    dword_t total = 0;

    while (local_index < liovcnt && remote_index < riovcnt) {
        while (local_index < liovcnt && local_iov[local_index].len == local_off) {
            local_index++;
            local_off = 0;
        }
        while (remote_index < riovcnt && remote_iov[remote_index].len == remote_off) {
            remote_index++;
            remote_off = 0;
        }
        if (local_index >= liovcnt || remote_index >= riovcnt)
            break;

        size_t local_left = local_iov[local_index].len - local_off;
        size_t remote_left = remote_iov[remote_index].len - remote_off;
        size_t chunk = local_left < remote_left ? local_left : remote_left;
        if (chunk == 0)
            break;

        char buf[4096];
        size_t done = 0;
        while (done < chunk) {
            size_t step = chunk - done;
            if (step > sizeof(buf))
                step = sizeof(buf);
            if (user_read_task(task, remote_iov[remote_index].base + remote_off + done, buf, step)) {
                free(local_iov);
                free(remote_iov);
                return total ? total : _EFAULT;
            }
            if (user_write(local_iov[local_index].base + local_off + done, buf, step)) {
                free(local_iov);
                free(remote_iov);
                return total ? total : _EFAULT;
            }
            done += step;
            total += step;
        }

        local_off += chunk;
        remote_off += chunk;
    }

    free(local_iov);
    free(remote_iov);
    return total;
}
