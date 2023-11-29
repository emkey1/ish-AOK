//
//  ro_locks.h
//  iSH-AOK
//
//  Created by Michael Miller on 11/29/23.
//

#ifndef ro_locks_h
#define ro_locks_h


#endif /* ro_locks_h */

#include <pthread.h>
#include <stdbool.h>

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
void atomic_l_lockf(char lname[16], int skiplog, const char *file, int line);
void atomic_l_unlockf(void);
void complex_lockt(lock_t *lock, int log_lock, const char *file, int line);
int trylock(lock_t *lock, const char *file, int line);
int trylocknl(lock_t *lock, char *comm, int pid, const char *file, int line);

#endif // RO_LOCK_H

