//
//  rw_locks.h
//  iSH-AOK
//
//  Created by Michael Miller on 11/29/23.
//

#ifndef rw_locks_h
#define rw_locks_h


#endif /* rw_locks_h */

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
void read_lock(wrlock_t *lock, const char *file, int line);
void write_lock(wrlock_t *lock, const char *file, int line);
void read_unlock(wrlock_t *lock, const char *file, int line);
void write_unlock(wrlock_t *lock, const char *file, int line);
void read_to_write_lock(wrlock_t *lock);
void write_to_read_lock(wrlock_t *lock, const char *file, int line);
void write_unlock_and_destroy(wrlock_t *lock);
void read_unlock_and_destroy(wrlock_t *lock);
void lock_destroy(wrlock_t *lock);

#endif // RW_LOCK_H
