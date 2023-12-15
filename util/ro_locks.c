//
//  ro_locks.c
//  iSH-AOK
//
//  Created by Michael Miller on 11/29/23.
//

#include <strings.h>
#include "misc.h"
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "kernel/log.h"
#include "util/sync.h"

// The following are in log.c.  There should probably be in a log.h that gets included instead.
bool current_is_valid(void);

// Lock to lock locks.  Used to assure transition between RO<->RW is automic for RW locks
lock_t atomic_l_lock;

// Function signatures and placeholders for implementation

void lock_init(lock_t *lock, char lname[16]) {
    int ret = pthread_mutex_init(&lock->m, NULL);
    if (ret != 0) {
        // Handle the error according to your application's needs
        printk("ERROR: Failed to initialize mutex: %s:(%s)\n", lname, strerror(ret));
        // Depending on how critical this failure is, you might choose to exit, return, or take other actions.
        return;
    }
    
    if(lname != NULL) {
        strncpy(lock->lname, lname, 16);
    } else {
        strncpy(lock->lname, "WTF", 16);
    }
    lock->wait4 = false;
#if LOCK_DEBUG
    lock->debug = (struct lock_debug) {
        .initialized = true,
    };
#endif
    lock->comm[0] = 0;
    lock->uid = -1;
}

