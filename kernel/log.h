//
//  log.h
//  iSH-AOK
//
//  Created by Michael Miller on 12/10/23.
//

#ifndef log_h
#define log_h

// Function prototypes
size_t sys_syslog(int_t type, addr_t buf_addr, int_t len);
void ish_vprintk(const char *msg, va_list args);
void ish_printk(const char *msg, ...);
_Noreturn void die(const char *msg, ...);

#endif /* log_h */
