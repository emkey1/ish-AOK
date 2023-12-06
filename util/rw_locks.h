//
//  rw_locks.h
//  iSH-AOK
//
//  Created by Michael Miller on 11/29/23.
//

#ifndef RW_LOCK_H
#define RW_LOCK_H

#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    pthread_rwlock_t l;
    atomic_int val;
    int favor_read;
    const char *file;
    int line;
    int pid;
    char comm[16];
    char lname[16];
} wrlock_t;

void wrlock_init(wrlock_t *lock);
void read_lock(wrlock_t *lock);
void write_lock(wrlock_t *lock);
void read_unlock(wrlock_t *lock);
void write_unlock(wrlock_t *lock);
void read_to_write_lock(wrlock_t *lock);
void write_to_read_lock(wrlock_t *lock);
void write_unlock_and_destroy(wrlock_t *lock);
void read_unlock_and_destroy(wrlock_t *lock);
void read_to_write_lock(wrlock_t *lock);
void read_unlock_and_destroy(wrlock_t *lock);
// void lock_destroy(wrlock_t *lock); // Not used outside of rw_locks.c, no need to be exposed
void handle_lock_error(wrlock_t *lock, const char *func);
int trylockw(wrlock_t *lock);

#define loop_lock_read(lock) loop_lock_generic(lock, 0)
#define loop_lock_write(lock) loop_lock_generic(lock, 1)

#endif // RW_LOCK_H
