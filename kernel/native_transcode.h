// transcode: a native program that re-encodes a video on the device's
// hardware encoder (kernel/native_transcode.c is the program, app/Transcode.m
// the AVFoundation half, kernel/native_transcode_glue.c what joins them).
//
// The program is compiled with the native libc shim, so everything it does is
// the guest's: its files, its descriptors, its signals. AVFoundation needs
// HOST files, which is what the glue is for -- it runs without the shim, so it
// can name the host file behind a guest descriptor and read a host file back.
#ifndef KERNEL_NATIVE_TRANSCODE_H
#define KERNEL_NATIVE_TRANSCODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum transcode_codec {
    TRANSCODE_HEVC = 0,
    TRANSCODE_H264 = 1,
};

struct transcode_job {
    // In: the source, a host path, and the codec and limits asked for.
    const char *in_host_path;
    enum transcode_codec codec;
    int max_lines;          // shorter side of the picture, 0 = keep
    long video_bitrate;     // bits per second, 0 = chosen from the picture
    double rate_scale;      // multiplies the chosen rate (0 = 1); ignored with video_bitrate
    long audio_bitrate;     // bits per second, used only when audio is re-encoded
    bool mov;               // QuickTime rather than MPEG-4
    bool no_audio;

    // Called on the program's own thread, about twice a second, while the
    // hook waits. should_cancel returning true cancels the job.
    void (*progress)(double fraction, void *ctx);
    bool (*should_cancel)(void *ctx);
    void *ctx;

    // Out: the host file written, which the program copies into the guest
    // and then removes, and a description of what was done or what failed.
    char out_host_path[1024];
    char summary[512];
    char error[512];
};

// 0 on success; -1 with job->error set; 1 when cancelled. NULL when the app
// has not installed it (the command-line ish).
extern int (*transcode_hook)(struct transcode_job *job);

// Where the out_host_path files go, so the app can clear any a crash left.
#define TRANSCODE_TEMP_PREFIX "transcode-"

// The host path of the regular file behind guest descriptor fd, for the
// calling task. -1 when fd is not a regular file kept in a host file (a pipe,
// tmpfs, /proc).
int transcode_fd_host_path(int fd, char *buf, size_t size);

// Host-side file access for the copy back. These are the host's open, read,
// close and unlink, which the program cannot call by name: the shim turns
// those into the guest's.
int transcode_host_open(const char *path);
ssize_t transcode_host_read(int host_fd, void *buf, size_t size);
void transcode_host_close(int host_fd);
void transcode_host_unlink(const char *path);

#endif
