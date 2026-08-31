//
//  log.h
//  iSH-AOK
//
//  Created by Michael Miller on 12/10/23.
//

#ifndef log_h
#define log_h

#include <stdarg.h>
#include <sys/types.h>
#include "misc.h"

// Function prototypes
size_t sys_syslog(int_t type, addr_t buf_addr, int_t len);
size_t sys_syslog_guest(int_t type, guest_addr_t buf_addr, int_t len);
size_t ish_log_size(void);
ssize_t ish_log_read_bytes(size_t offset, void *buf, size_t len);
// Streaming reads of the kernel log, positioned against a monotonic count of
// every byte ever logged rather than an offset into the ring buffer. Backs
// /dev/kmsg and /proc/kmsg, both of which a log daemon sits in a blocking
// read on.
uint64_t ish_log_total_written(void);
ssize_t ish_log_read_at(uint64_t *pos, void *buf, size_t len);
int ish_log_wait_past(uint64_t pos);
void ish_vprintk(const char *msg, va_list args);
void ish_printk(const char *msg, ...);
__attribute__((__noreturn__)) void die(const char *msg, ...);

#endif /* log_h */
