#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/uio.h>
#if LOG_HANDLER_NSLOG
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "kernel/calls.h"
#include "util/sync.h"
#include "util/fifo.h"
#include "kernel/task.h"
#include "kernel/resource_locking.h"
#include "misc.h"

#define LOG_BUF_SHIFT 20
static char log_buffer[1 << LOG_BUF_SHIFT];
static struct fifo log_buf = FIFO_INIT(log_buffer);
static size_t log_max_since_clear = 0;
static lock_t log_lock = LOCK_INITIALIZER;

#define SYSLOG_ACTION_CLOSE_ 0
#define SYSLOG_ACTION_OPEN_ 1
#define SYSLOG_ACTION_READ_ 2
#define SYSLOG_ACTION_READ_ALL_ 3
#define SYSLOG_ACTION_READ_CLEAR_ 4
#define SYSLOG_ACTION_CLEAR_ 5
#define SYSLOG_ACTION_CONSOLE_OFF_ 6
#define SYSLOG_ACTION_CONSOLE_ON_ 7
#define SYSLOG_ACTION_CONSOLE_LEVEL_ 8
#define SYSLOG_ACTION_SIZE_UNREAD_ 9
#define SYSLOG_ACTION_SIZE_BUFFER_ 10

static size_t syslog_read(addr_t buf_addr, size_t len, int flags) {
    if (len < 0)
        return _EINVAL;
    if (flags & FIFO_LAST) {
        if ((size_t) len > log_max_since_clear)
            len = log_max_since_clear;
    } else {
        if ((size_t) len > fifo_capacity(&log_buf))
            len = fifo_capacity(&log_buf);
    }
    char *buf = malloc(len + 1);
    fifo_read(&log_buf, buf, len, flags);
    
    // Here we will split on \n and do one entry per line
    // Keep printing tokens while one of the
    // delimiters present
    addr_t pointer = buf_addr; // Where we are in the buffer
    char *token = strtok(buf, "\n"); // Get the first line
    
    if(user_write(pointer, "\n", 1)) { // Positive return value = fail
        free(buf);
        return _EFAULT;
    }
    
    pointer++;
    
    while (token != NULL) {
        size_t length = strlen(token);
        if(user_write(pointer, token, length)) { // Positive return value = fail
            free(buf);
            return _EFAULT;
        }
           
        pointer += length;
        
        if(user_write(pointer, "\n", 1)) { // Positive return value = fail
            free(buf);
            return _EFAULT;
        }
        
        pointer++;
        if(pointer < (buf_addr + (len -1))) {
            token = strtok(NULL, "\n");  // Grab next token, deal with when back at top of while loop. -mke
        } else {
            token = NULL;
        }
    }

    free(buf);
    
    return len;
}

static size_t do_syslog(int type, addr_t buf_addr, int_t len) {
    size_t res;
    switch (type) {
        case SYSLOG_ACTION_READ_:
            return syslog_read(buf_addr, len, 0);
        case SYSLOG_ACTION_READ_ALL_:
            return syslog_read(buf_addr, len, FIFO_LAST | FIFO_PEEK);

        case SYSLOG_ACTION_READ_CLEAR_:
            res = syslog_read(buf_addr, len, FIFO_LAST | FIFO_PEEK);
            if (res < 0)
                return res;
            fallthrough;
        case SYSLOG_ACTION_CLEAR_:
            log_max_since_clear = 0;
            return 0;

        case SYSLOG_ACTION_SIZE_UNREAD_:
            return (int)fifo_size(&log_buf);
        case SYSLOG_ACTION_SIZE_BUFFER_:
            return (int)fifo_capacity(&log_buf);

        case SYSLOG_ACTION_CLOSE_:
        case SYSLOG_ACTION_OPEN_:
        case SYSLOG_ACTION_CONSOLE_OFF_:
        case SYSLOG_ACTION_CONSOLE_ON_:
        case SYSLOG_ACTION_CONSOLE_LEVEL_:
            return 0;
        default:
            return _EINVAL;
    }
}
size_t sys_syslog(int_t type, addr_t buf_addr, int_t len) {
    ////modify_critical_region_counter(current, 1, __FILE__, __LINE__);
    lock(&log_lock, 0);
    size_t retval = do_syslog(type, buf_addr, len);
    unlock(&log_lock);
    ////modify_critical_region_counter(current, -1, __FILE__, __LINE__);
    return retval;
}

static void log_buf_append(const char *msg) {
    fifo_write(&log_buf, msg, strlen(msg), FIFO_OVERWRITE);
    log_max_since_clear += strlen(msg);
    if (log_max_since_clear > fifo_capacity(&log_buf))
        log_max_since_clear = fifo_capacity(&log_buf);
}

static void log_line(const char *line);

/* static void output_line(const char *line) {
    time_t t=time(NULL);
    char* c_time_string;
    c_time_string = ctime(&t);
    c_time_string[strcspn(c_time_string, "\n")] = 0;  // Remove trailing newline
    //double tstamp = difftime(t, (time_t) 0);
    int mybuff_size = 512;
    char tmpbuff[mybuff_size];
    //sprintf(tmpbuff, "[   %f] %s", tstamp, line);
    sprintf(tmpbuff, "[   %s] %s", c_time_string, line);
    // send it to stdout or wherever
    if(strcmp(tmpbuff, "") != 0) { // Don't log empty string
        log_line(tmpbuff);
        // add it to the circular buffer
        log_buf_append(tmpbuff);
        log_buf_append("\n");
    }
} */
static void output_line(const char *line) {
     time_t t = time(NULL);
     char* c_time_string = ctime(&t);
     const size_t tlen = strlen(c_time_string); // We can trust c_time_string to be null terminated
     c_time_string[tlen - 1] = '\0'; // Remove trailing newline

     char tmpbuff[512];
     if (snprintf(tmpbuff, 512, "[   %s] %s", c_time_string, line) >= 512) { // Insufficient room, need to terminate at buffer size
         tmpbuff[511] = '\0';
     }
    // send it to stdout or wherever
    if(strcmp(tmpbuff, "") != 0) { // Don't log empty string
        log_line(tmpbuff);
        // add it to the circular buffer
        log_buf_append(tmpbuff);
        log_buf_append("\n");
    }
}

void ish_vprintk(const char *msg, va_list args) {
    // format the message
    // I'm trusting you to not pass an absurdly long message
    static __thread char buf[16384] = "";
    static __thread size_t buf_size = 0;
    
    buf_size += vsprintf(buf + buf_size, msg, args);

    // output up to the last newline, leave the rest in the buffer
    complex_lockt(&log_lock, 1, __FILE__, __LINE__);
    char *b = buf;
    char *p;
    while ((p = strchr(b, '\n')) != NULL) {
        *p = '\0';
        output_line(b);
        *p = '\n';
        buf_size -= p + 1 - b;
        b = p + 1;
    }
    unlock(&log_lock);
    memmove(buf, b, strlen(b) + 1);
}

void ish_printk(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    ish_vprintk(msg, args);
    va_end(args);
}

#if LOG_HANDLER_DPRINTF
#define NEWLINE "\r\n"
static void log_line(const char *line) {
    struct iovec output[2] = {{(void *) line, strlen(line)}, {"\n", 1}};
    writev(666, output, 2);
}
#elif LOG_HANDLER_NSLOG
static void log_line(const char *line) {
    extern void NSLog(CFStringRef msg, ...);
    if(strcmp(line, "") != 0) // Don't log empty string
        NSLog(CFSTR("%s"), line);
}
#endif

static void default_die_handler(const char *msg) {
    printk("%s\n", msg);
}
void (*die_handler)(const char *msg) = default_die_handler;
_Noreturn void die(const char *msg, ...);
void die(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    char buf[4096];
    vsprintf(buf, msg, args);
    die_handler(buf);
    abort();  
    va_end(args);
}

// fun little utility function
int current_pid(void) {
    modify_critical_region_counter(current, 1, __FILE__, __LINE__);
    if(current != NULL) {
        if (current->exiting != true) {
            modify_critical_region_counter(current, -1, __FILE__, __LINE__);
            return current->pid;
        } else {
            modify_critical_region_counter(current, -1, __FILE__, __LINE__);
            return -1;
        }
    }
    
    modify_critical_region_counter(current, -1, __FILE__, __LINE__);
    return -1;
}

int current_uid(void) {
    modify_critical_region_counter(current, 1, __FILE__, __LINE__);
    if(current != NULL) {
        if (current->exiting != true) {
            modify_critical_region_counter(current, -1, __FILE__, __LINE__);
            return current->uid;
        } else {
            modify_critical_region_counter(current, -1, __FILE__, __LINE__);
            return -1;
        }
    }
    
    modify_critical_region_counter(current, -1, __FILE__, __LINE__);
    return -1;
}

char * current_comm(void) {
    static char comm[16];
    if(current != NULL) {
        if(strcmp(current->comm, "")) {
            strncpy(comm, current->comm, 16);
        } else {
            return "";
        }
        if (current->exiting != true) {
            return comm;
        } else {
            return "";
        }
    }
    
    return "";
}


