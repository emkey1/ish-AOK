// transcode -- re-encode a video on the device's hardware encoder.
//
// A native program (kernel/native.c), compiled with the native libc shim like
// motepad: its files, descriptors and signals are the guest's. The encoding
// is AVFoundation's, in the app (app/Transcode.m), reached through
// transcode_hook; kernel/native_transcode.h explains the split.
//
// Why it exists: ffmpeg in the guest encodes in emulated software, which is
// about the slowest thing an emulator can be asked to do -- H.265 ran at
// 0.005x real time on a phone that encodes it in hardware at many times real
// time. This hands the whole job to that hardware.
//
// Kept to one invocation's state: a native program is a function call in a
// process that outlives it, so nothing here is static.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "kernel/native_transcode.h"

int native_transcode_main(int argc, char *const argv[], char *const envp[]);

struct tc_run {
    bool show_progress;
    int last_percent;
    struct timespec start;
    sigset_t stop_signals;
};

static void tc_usage(FILE *out) {
    fprintf(out,
        "usage: transcode [options] INPUT OUTPUT\n"
        "Re-encode a video on the device's hardware video encoder.\n"
        "\n"
        "  -c CODEC   hevc (default) or h264\n"
        "  -s LINES   shrink so the shorter side is at most LINES (e.g. 1080, 720)\n"
        "  -b RATE    video bitrate, e.g. 2M or 1500k (default: chosen from the size,\n"
        "             and never above the source's)\n"
        "  -a RATE    audio bitrate when the audio has to be re-encoded (default 128k)\n"
        "  -n         drop the audio\n"
        "  -y         overwrite OUTPUT if it exists\n"
        "  -q         no progress line\n"
        "  -h         this help\n"
        "\n"
        "OUTPUT ending in .mov is QuickTime; anything else is MPEG-4.\n"
        "INPUT must be an ordinary file (not a pipe, and not on tmpfs).\n"
        "See /AOK/docs/transcode.md.\n");
}

static bool tc_parse_rate(const char *s, long *out) {
    char *end;
    double v = strtod(s, &end);
    if (end == s || v <= 0)
        return false;
    if (*end == 'k' || *end == 'K')
        v *= 1000, end++;
    else if (*end == 'm' || *end == 'M')
        v *= 1000000, end++;
    if (*end != '\0' || v < 8000 || v > 1e9)
        return false;
    *out = (long) v;
    return true;
}

static double tc_elapsed(const struct tc_run *run) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double) (now.tv_sec - run->start.tv_sec) +
        (double) (now.tv_nsec - run->start.tv_nsec) / 1e9;
}

static void tc_progress(double fraction, void *ctx) {
    struct tc_run *run = ctx;
    if (!run->show_progress)
        return;
    int percent = (int) (fraction * 100);
    if (percent > 100)
        percent = 100;
    if (percent == run->last_percent)
        return;
    run->last_percent = percent;
    double t = tc_elapsed(run);
    fprintf(stderr, "\r%3d%%  %d:%02d", percent, (int) t / 60, (int) t % 60);
    if (fraction > 0.02) {
        double left = t / fraction - t;
        fprintf(stderr, "  about %d:%02d left ", (int) left / 60, (int) left % 60);
    }
    fflush(stderr);
}

static bool tc_should_cancel(void *ctx) {
    struct tc_run *run = ctx;
    sigset_t pending;
    if (sigpending(&pending) != 0)
        return false;
    for (int sig = 1; sig < NSIG; sig++)
        if (sigismember(&run->stop_signals, sig) && sigismember(&pending, sig))
            return true;
    return false;
}

static bool tc_has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcasecmp(s + n - m, suffix) == 0;
}

// Copies the host file at host_path into guest descriptor out.
static int tc_copy_back(const char *host_path, int out, const char *out_name) {
    int in = transcode_host_open(host_path);
    if (in < 0) {
        fprintf(stderr, "transcode: cannot read the encoded file: %s\n", strerror(errno));
        return -1;
    }
    char *buf = malloc(1 << 20);
    int res = 0;
    if (buf == NULL) {
        fprintf(stderr, "transcode: out of memory\n");
        res = -1;
    }
    while (res == 0) {
        ssize_t n = transcode_host_read(in, buf, 1 << 20);
        if (n < 0) {
            fprintf(stderr, "transcode: cannot read the encoded file: %s\n", strerror(errno));
            res = -1;
            break;
        }
        if (n == 0)
            break;
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buf + done, (size_t) (n - done));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                fprintf(stderr, "transcode: %s: %s\n", out_name, strerror(errno));
                res = -1;
                break;
            }
            done += w;
        }
    }
    free(buf);
    transcode_host_close(in);
    return res;
}

static void tc_size(char *buf, size_t size, long long bytes) {
    if (bytes >= 1000LL * 1000 * 1000)
        snprintf(buf, size, "%.2f GB", bytes / 1e9);
    else
        snprintf(buf, size, "%.1f MB", bytes / 1e6);
}

static int tc_main(int argc, char *const argv[]) {
    struct transcode_job job;
    memset(&job, 0, sizeof(job));
    job.codec = TRANSCODE_HEVC;
    job.audio_bitrate = 128000;
    bool overwrite = false, quiet = false;
    const char *paths[2];
    int npaths = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0' || npaths > 0) {
            if (npaths == 2) {
                fprintf(stderr, "transcode: too many arguments\n");
                return 2;
            }
            paths[npaths++] = a;
            continue;
        }
        if (strcmp(a, "--") == 0) {
            for (i++; i < argc; i++) {
                if (npaths == 2) {
                    fprintf(stderr, "transcode: too many arguments\n");
                    return 2;
                }
                paths[npaths++] = argv[i];
            }
            break;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            tc_usage(stdout);
            return 0;
        }
        if (strcmp(a, "-y") == 0) { overwrite = true; continue; }
        if (strcmp(a, "-q") == 0) { quiet = true; continue; }
        if (strcmp(a, "-n") == 0) { job.no_audio = true; continue; }
        if (a[2] != '\0' || strchr("csba", a[1]) == NULL) {
            fprintf(stderr, "transcode: unknown option %s\n", a);
            tc_usage(stderr);
            return 2;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "transcode: %s needs a value\n", a);
            return 2;
        }
        const char *v = argv[++i];
        switch (a[1]) {
        case 'c':
            if (strcasecmp(v, "hevc") == 0 || strcasecmp(v, "h265") == 0)
                job.codec = TRANSCODE_HEVC;
            else if (strcasecmp(v, "h264") == 0 || strcasecmp(v, "avc") == 0)
                job.codec = TRANSCODE_H264;
            else {
                fprintf(stderr, "transcode: codec must be hevc or h264\n");
                return 2;
            }
            break;
        case 's': {
            char *end;
            long n = strtol(v, &end, 10);
            if (*end == 'p')
                end++;
            if (*end != '\0' || n < 16 || n > 8192) {
                fprintf(stderr, "transcode: -s wants a number of lines, such as 720\n");
                return 2;
            }
            job.max_lines = (int) n;
            break;
        }
        case 'b':
            if (!tc_parse_rate(v, &job.video_bitrate)) {
                fprintf(stderr, "transcode: -b wants a rate such as 2M or 1500k\n");
                return 2;
            }
            break;
        case 'a':
            if (!tc_parse_rate(v, &job.audio_bitrate)) {
                fprintf(stderr, "transcode: -a wants a rate such as 128k\n");
                return 2;
            }
            break;
        }
    }
    if (npaths != 2) {
        tc_usage(stderr);
        return 2;
    }
    const char *in_name = paths[0], *out_name = paths[1];
    job.mov = tc_has_suffix(out_name, ".mov");

    if (transcode_hook == NULL) {
        fprintf(stderr, "transcode: needs the iSH-AOK app (the encoder is iOS's)\n");
        return 1;
    }

    int in = open(in_name, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "transcode: %s: %s\n", in_name, strerror(errno));
        return 1;
    }
    struct stat in_st, out_st;
    if (fstat(in, &in_st) != 0 || !S_ISREG(in_st.st_mode)) {
        fprintf(stderr, "transcode: %s: not a regular file\n", in_name);
        close(in);
        return 1;
    }
    char in_host[1024];
    if (transcode_fd_host_path(in, in_host, sizeof(in_host)) != 0) {
        fprintf(stderr, "transcode: %s: not on a file system iOS can read directly"
                " (tmpfs?); copy it to your home directory first\n", in_name);
        close(in);
        return 1;
    }
    if (stat(out_name, &out_st) == 0) {
        if (out_st.st_dev == in_st.st_dev && out_st.st_ino == in_st.st_ino) {
            fprintf(stderr, "transcode: OUTPUT is the same file as INPUT\n");
            close(in);
            return 1;
        }
        if (!overwrite) {
            fprintf(stderr, "transcode: %s exists (-y to overwrite)\n", out_name);
            close(in);
            return 1;
        }
    }
    job.in_host_path = in_host;

    // Ctrl-C and friends are held while the encoder runs and looked for twice
    // a second, so that a cancel stops the encoder and removes its file
    // before the signal does what it does. The default action of an unheld
    // one would end this function with AVFoundation still writing.
    struct tc_run run = { .show_progress = !quiet && isatty(2), .last_percent = -1 };
    sigemptyset(&run.stop_signals);
    sigaddset(&run.stop_signals, SIGINT);
    sigaddset(&run.stop_signals, SIGTERM);
    sigaddset(&run.stop_signals, SIGHUP);
    sigaddset(&run.stop_signals, SIGQUIT);
    sigset_t old_mask;
    sigprocmask(SIG_BLOCK, &run.stop_signals, &old_mask);
    clock_gettime(CLOCK_MONOTONIC, &run.start);
    job.progress = tc_progress;
    job.should_cancel = tc_should_cancel;
    job.ctx = &run;

    int res = transcode_hook(&job);
    if (run.show_progress && run.last_percent >= 0)
        fprintf(stderr, "\r\033[K");
    double seconds = tc_elapsed(&run);

    int status = 0;
    if (res == 1) {
        fprintf(stderr, "transcode: cancelled\n");
        status = 130;
    } else if (res != 0) {
        fprintf(stderr, "transcode: %s\n", job.error[0] ? job.error : "the encoder failed");
        status = 1;
    } else {
        int out = open(out_name, O_WRONLY | O_CREAT | O_TRUNC | (overwrite ? 0 : O_EXCL), 0666);
        if (out < 0) {
            fprintf(stderr, "transcode: %s: %s\n", out_name, strerror(errno));
            status = 1;
        } else {
            if (tc_copy_back(job.out_host_path, out, out_name) != 0)
                status = 1;
            if (close(out) != 0 && status == 0) {
                fprintf(stderr, "transcode: %s: %s\n", out_name, strerror(errno));
                status = 1;
            }
            if (status != 0)
                unlink(out_name);
        }
        if (status == 0 && !quiet) {
            char a[32], b[32];
            tc_size(a, sizeof(a), in_st.st_size);
            if (stat(out_name, &out_st) != 0)
                out_st.st_size = 0;
            tc_size(b, sizeof(b), out_st.st_size);
            printf("%s: %s -> %s (%.0f%%) in %.1f s; %s\n", out_name, a, b,
                in_st.st_size > 0 ? 100.0 * out_st.st_size / in_st.st_size : 0.0,
                seconds, job.summary);
            if (out_st.st_size > in_st.st_size)
                printf("transcode: note: %s is larger than %s; the source was already more"
                       " compact than this setting (try -s or a lower -b)\n", out_name, in_name);
        }
    }
    if (job.out_host_path[0] != '\0')
        transcode_host_unlink(job.out_host_path);
    close(in);
    // Lets a held Ctrl-C do what it would have done.
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return status;
}

int native_transcode_main(int argc, char *const argv[], char *const envp[]) {
    (void) envp;
    int status = tc_main(argc, argv);
    // A native program returns into a process that keeps running, so nothing
    // flushes stdio for it: stdout to a pipe or file is fully buffered, and
    // the help and the summary line were lost without this.
    fflush(stdout);
    fflush(stderr);
    return status;
}
