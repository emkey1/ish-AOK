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

static size_t syslog_read(guest_addr_t buf_addr, size_t len, int flags) {
    size_t available = fifo_size(&log_buf);
    if (flags & FIFO_LAST && available > log_max_since_clear)
        available = log_max_since_clear;
    if (len > available)
        len = available;
    if (len == 0)
        return 0;

    char *buf = malloc(len);
    if (buf == NULL)
        return _ENOMEM;
    if (fifo_read(&log_buf, buf, len, flags)) {
        free(buf);
        return _EIO;
    }
    if (user_write(buf_addr, buf, len)) {
        free(buf);
        return _EFAULT;
    }
    free(buf);
    return len;
}

size_t ish_log_size(void) {
    lock(&log_lock, 0);
    size_t size = fifo_size(&log_buf);
    unlock(&log_lock);
    return size;
}

ssize_t ish_log_read_bytes(size_t offset, void *buf, size_t len) {
    lock(&log_lock, 0);
    size_t available = fifo_size(&log_buf);
    if (offset >= available) {
        unlock(&log_lock);
        return 0;
    }
    if (len > available - offset)
        len = available - offset;
    if (len == 0) {
        unlock(&log_lock);
        return 0;
    }

    size_t start = (log_buf.start + offset) % log_buf.capacity;
    size_t first_copy_size = log_buf.capacity - start;
    if (first_copy_size > len)
        first_copy_size = len;
    memcpy(buf, &log_buf.buf[start], first_copy_size);
    memcpy((char *) buf + first_copy_size, &log_buf.buf[0], len - first_copy_size);
    unlock(&log_lock);
    return (ssize_t) len;
}

static size_t do_syslog(int type, guest_addr_t buf_addr, int_t len) {
    int res;
    switch (type) {
        case SYSLOG_ACTION_READ_:
            if (len < 0)
                return _EINVAL;
            return syslog_read(buf_addr, len, 0);
        case SYSLOG_ACTION_READ_ALL_:
            if (len < 0)
                return _EINVAL;
            return syslog_read(buf_addr, len, FIFO_LAST | FIFO_PEEK);

        case SYSLOG_ACTION_READ_CLEAR_:
            if (len < 0)
                return _EINVAL;
            res = (int)syslog_read(buf_addr, len, FIFO_LAST | FIFO_PEEK);
            if (res < 0)
                return res;
            FALLTHROUGH;
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
    return sys_syslog_guest(type, buf_addr, len);
}

size_t sys_syslog_guest(int_t type, guest_addr_t buf_addr, int_t len) {
    lock(&log_lock, 0);
    size_t retval = do_syslog(type, buf_addr, len);
    unlock(&log_lock);
    return retval;
}

static void log_buf_append(const char *msg) {
    fifo_write(&log_buf, msg, strlen(msg), FIFO_OVERWRITE);
    log_max_since_clear += strlen(msg);
    if (log_max_since_clear > fifo_capacity(&log_buf))
        log_max_since_clear = fifo_capacity(&log_buf);
}

static void log_line(const char *line);

static void output_line(const char *line) {
     if (strncmp(line, "INFO:", 5) == 0)
         return;
     time_t t = time(NULL);
     char* c_time_string = ctime(&t);
     const size_t tlen = strlen(c_time_string); // We can trust c_time_string to be null terminated
     c_time_string[tlen - 1] = '\0'; // Remove trailing newline

     char tmpbuff[16384];
     if (snprintf(tmpbuff, sizeof(tmpbuff), "[%s] %s", c_time_string, line) >= (int) sizeof(tmpbuff)) { // Insufficient room, need to terminate at buffer size
         tmpbuff[sizeof(tmpbuff) - 1] = '\0';
     }
    // send it to stdout or wherever
    if(tmpbuff[0] != '\0') { // Don't log empty string
        log_line(tmpbuff);
        // add it to the circular buffer
        log_buf_append(tmpbuff);
        log_buf_append("\n");
    }
}

void ish_vprintk(const char *msg, va_list args) {
    // format the message
    static __thread char buf[16384] = "";
    static __thread size_t buf_size = 0;

    size_t available = sizeof(buf) - buf_size;
    if (available > 0) {
        int ret = vsnprintf(buf + buf_size, available, msg, args);
        if (ret > 0) {
            if ((size_t)ret >= available)
                buf_size = sizeof(buf) - 1;
            else
                buf_size += ret;
        }
    }

    // output up to the last newline, leave the rest in the buffer
    complex_lockt(&log_lock, 1);
    char *b = buf;
    char *p;
    while ((p = strchr(b, '\n')) != NULL) {
        *p = '\0';
        output_line(b);
        *p = '\n';
        buf_size -= p + 1 - b;
        b = p + 1;
    }

    if (buf_size >= sizeof(buf) - 1) {
        output_line(b);
        buf_size = 0;
        b = buf + sizeof(buf) - 1;
        buf[0] = '\0';
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
    writev(555, output, 2);
}
#elif LOG_HANDLER_NSLOG
static void log_line(const char *line) {
    extern void NSLog(CFStringRef msg, ...);
    if(line[0] != '\0') // Don't log empty string
        NSLog(CFSTR("%s"), line);
}
#endif

static void default_die_handler(const char *msg) {
    printk("%s\n", msg);
}
void (*die_handler)(const char *msg) = default_die_handler;
__attribute__((__noreturn__)) void die(const char *msg, ...);
void die(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), msg, args);
    die_handler(buf);
    abort();  
    va_end(args);
}
