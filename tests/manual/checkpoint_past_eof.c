// checkpoint_past_eof.c -- a file mapping longer than its file, across a
// checkpoint. Driven by checkpoint_past_eof.sh: it needs ISH_GUEST_CHECKPOINT=1
// or ISH_CHECKPOINT_AFTER, and its second life is a separate ish run from the
// image (ISH_RESTORE). Deliberately does not include test_common.h, which would
// enlist it in suites that run it without either.
//
//     checkpoint_past_eof <image path>     asks for the save itself
//     checkpoint_past_eof --sleep <secs>   sleeps where the save would be, for
//                                          one taken from outside the guest
//
// A host page wholly past the end of a mapped file cannot be read: a load from
// it is SIGBUS. The save read every mapped page, so it killed the whole app the
// first time a process held such a page -- and ordinary processes do: musl's
// dynamic linker maps a library's full span from its file and maps the data
// segment over the far end, leaving the gap between as file offsets the file
// does not have. Any Python on the arm64 Alpine root that had imported math
// (69,672 bytes, mapped 132 KiB long) took the app down at every suspend.
//
// This maps a 5000-byte file 256 KiB long, which leaves whole host pages past
// the end of the file on any host with pages up to 64 KiB, and saves. Checked
// in both lives, and never by touching a page past the end, which is SIGBUS in
// the original life as it is on Linux:
//   FILE-BYTES  the 5000 bytes of the file, as written
//   TAIL-ZERO   the rest of the page holding the end of the file reads zero
//   MAPPING     all 256 KiB are still mapped, in /proc/self/maps
// Prints "life: original" or "life: restored", then RESULT: PASS or FAIL <n>.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define FILE_BYTES 5000
#define MAP_BYTES (256 * 1024)

static int failures;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "OK" : "FAIL", what);
    if (!ok)
        failures++;
}

static unsigned char pattern(size_t i) {
    return (unsigned char) (i * 7 + 3);
}

// Whether one region of /proc/self/maps covers all of [lo, hi). Not "is
// exactly": a restored mapping is anonymous, and an anonymous neighbour with
// the same protection may be shown merged with it, as Linux merges them.
static int mapping_covered(unsigned long lo, unsigned long hi) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int found = 0;
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        unsigned long s, e;
        if (sscanf(line, "%lx-%lx", &s, &e) == 2 && s <= lo && e >= hi)
            found = 1;
    }
    if (f != NULL)
        fclose(f);
    return found;
}

static int second_life(void) {
    char status[4096] = "";
    int fd = open("/proc/ish/checkpoint", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, status, sizeof(status) - 1);
        status[n > 0 ? n : 0] = '\0';
        close(fd);
    }
    for (char *l = status; l != NULL && *l != '\0'; ) {
        if (strncmp(l, "restored ", 9) == 0) {
            char *v = l + 9;
            while (*v == ' ')
                v++;
            return strncmp(v, "yes", 3) == 0;
        }
        l = strchr(l, '\n');
        if (l != NULL)
            l++;
    }
    return 0;
}

int main(int argc, char **argv) {
    int sleep_secs = argc == 3 && strcmp(argv[1], "--sleep") == 0 ? atoi(argv[2]) : -1;
    if (argc != 2 && sleep_secs < 0) {
        fprintf(stderr, "usage: %s <image path> | --sleep <seconds>\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);   // or a restore re-prints buffered lines

    char path[] = "/tmp/ckpt-past-eof-XXXXXX";
    int fd = mkstemp(path);
    unsigned char buf[FILE_BYTES];
    for (size_t i = 0; i < FILE_BYTES; i++)
        buf[i] = pattern(i);
    if (fd < 0 || write(fd, buf, FILE_BYTES) != FILE_BYTES) {
        printf("setup failed\nRESULT: FAIL setup\n");
        return 1;
    }
    unsigned char *map = mmap(NULL, MAP_BYTES, PROT_READ, MAP_PRIVATE, fd, 0);
    // As the dynamic linker does: the mapping outlives the descriptor.
    close(fd);
    unlink(path);
    if (map == MAP_FAILED) {
        printf("mmap failed\nRESULT: FAIL setup\n");
        return 1;
    }

    printf("A-BEFORE-SAVE\n");
    if (sleep_secs >= 0) {
        sleep((unsigned) sleep_secs);   // a restore resumes the sleep
    } else {
        fd = open("/proc/ish/checkpoint", O_WRONLY);
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "save %s\n", argv[1]);
        if (fd < 0 || write(fd, cmd, strlen(cmd)) < 0) {
            perror("checkpoint");
            printf("RESULT: FAIL no checkpoint\n");
            return 1;
        }
        close(fd);
    }
    printf("life: %s\n", second_life() ? "restored" : "original");

    int same = 1;
    for (size_t i = 0; i < FILE_BYTES; i++)
        if (map[i] != pattern(i))
            same = 0;
    check(same, "FILE-BYTES: the file's 5000 bytes are there");
    int zero = 1;
    for (size_t i = FILE_BYTES; i < 8192; i++)
        if (map[i] != 0)
            zero = 0;
    check(zero, "TAIL-ZERO: the rest of the last file page reads zero");
    check(mapping_covered((unsigned long) map, (unsigned long) map + MAP_BYTES),
          "MAPPING: all 256 KiB of the mapping are still mapped");

    if (failures == 0)
        printf("RESULT: PASS\n");
    else
        printf("RESULT: FAIL %d\n", failures);
    return failures != 0;
}
