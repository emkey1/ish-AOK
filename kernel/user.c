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
    mem_ref_cnt_mod(task->mem, 1);
    read_lock(&task->mem->lock);
    int res = __user_read_task(task, addr, buf, count);

    read_unlock(&task->mem->lock);
    mem_ref_cnt_mod(task->mem, -1);
    return res;
}

int user_read(addr_t addr, void *buf, size_t count) {
    return user_read_task(current, addr, buf, count);
}

int user_write_task(struct task *task, addr_t addr, const void *buf, size_t count) {
    read_lock(&task->mem->lock);
    task_ref_cnt_mod(current, 1);
    mem_ref_cnt_mod(current->mem, 1);
    int res = __user_write_task(task, addr, buf, count, false);
    read_unlock(&task->mem->lock);
    task_ref_cnt_mod(current, -1);
    mem_ref_cnt_mod(current->mem, -1);
    return res;
}

int user_write_task_ptrace(struct task *task, addr_t addr, const void *buf, size_t count) {
    read_lock(&task->mem->lock);
    task_ref_cnt_mod(current, 1);
    mem_ref_cnt_mod(current->mem, 1);
    int res = __user_write_task(task, addr, buf, count, true);
    read_unlock(&task->mem->lock);
    task_ref_cnt_mod(current, -1);
    mem_ref_cnt_mod(current->mem, -1);
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
        if (__user_read_task(current, addr + i, &buf[i], sizeof(buf[i])), false) {
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
