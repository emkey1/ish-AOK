//
//  ro_locks.h
//  iSH-AOK
//
//  Created by Michael Miller on 11/29/23.
//
#ifndef RO_LOCKS_H
#define RO_LOCKS_H

#include <pthread.h>
#include <stdbool.h>
#include <util/sync.h>

typedef struct {
    pthread_mutex_t m;
    pthread_t owner;
    int pid;
    int uid;
    char comm[16];
    char lname[16];
    bool wait4;
#if LOCK_DEBUG
    struct lock_debug {
        const char *file;
        int line;
        int pid;
        bool initialized;
    } debug;
#endif
} lock_t;


void lock_init(lock_t *lock, char lname[16]);
void unlock(lock_t *lock);
void atomic_l_lockf(char lname[16], int skiplog);
void mylock(lock_t *lock, int log_lock);
void atomic_l_unlockf(void);
void complex_lockt(lock_t *lock, int log_lock);
int trylock(lock_t *lock);
int trylocknl(lock_t *lock, char *comm, int pid);

#define lock(lock, log_lock) mylock(lock, log_lock)
//#define trylock(lock) trylock(lock, __FILE__, __LINE__)
//#define trylocknl(lock, comm, pid) trylocknl(lock, comm, pid, __FILE__, __LINE__)

#endif
