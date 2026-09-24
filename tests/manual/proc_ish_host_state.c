// proc_ish_host_state.c — the host state /proc/ish reports: the battery files,
// thermal_state, timezone, and the Workspace applets.
//
// The battery files used to come from printBatteryStatus(), which asked
// UIDevice on the reading guest thread and returned the UTF8String of a
// temporary NSString -- memory nothing kept alive once it returned. Formatting
// moved into the kernel, over a cache the app's main queue keeps, and the
// output was meant to stay exactly what scripts already read. So this checks
// the FORMAT, on every read of a burst from several threads at once: a torn or
// freed buffer shows up as a line that does not parse.
//
// thermal_state is iOS's coarse thermal state; the command-line build has no
// source and must say "unknown" rather than pick a state. timezone is the
// host's IANA zone name, which has to be usable as a path under
// /usr/share/zoneinfo -- that is what the app's boot-time provisioning and the
// provision-ultimate-* scripts do with it.
//
// AOK-only: skipped where there is no /proc/ish.
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_common.h"

#define READERS 4
#define READS_PER_READER 50

static int check(const char *label, int cond) {
    if (cond) {
        test_logf("ok   %s\n", label);
        return 1;
    }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

static ssize_t slurp(const char *path, char *buf, size_t bufsize) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t total = 0;
    while (total < bufsize - 1) {
        ssize_t n = read(fd, buf + total, bufsize - 1 - total);
        if (n <= 0)
            break;
        total += (size_t) n;
    }
    close(fd);
    buf[total] = '\0';
    return (ssize_t) total;
}

static int is_cli(void) {
    char buf[64];
    return slurp("/proc/ish/UIDevice", buf, sizeof(buf)) > 0 && strncmp(buf, "standalone", 10) == 0;
}

// "host device: Darwin" from the command-line build on a Mac.
static int host_is_darwin(void) {
    char buf[1024];
    return slurp("/proc/ish/host_info", buf, sizeof(buf)) > 0 && strstr(buf, "host device: Darwin") != NULL;
}

// "-100.00", "83.00": optional minus, digits, a point, exactly two digits.
static int two_decimal_number(const char *s, size_t len) {
    size_t i = 0;
    if (i < len && s[i] == '-')
        i++;
    size_t digits = 0;
    while (i < len && isdigit((unsigned char) s[i])) {
        i++;
        digits++;
    }
    return digits > 0 && i + 3 == len && s[i] == '.' &&
           isdigit((unsigned char) s[i + 1]) && isdigit((unsigned char) s[i + 2]);
}

static int battery_state_word(const char *s) {
    return strcmp(s, "Unknown") == 0 || strcmp(s, "Discharging") == 0 ||
           strcmp(s, "Charging") == 0 || strcmp(s, "Full") == 0;
}

// One /proc/ish/BAT0 read: exactly three lines, in order. 0 when it parses,
// with the state copied out.
static int parse_bat0(const char *text, char *state, size_t statesize, char *lpm, size_t lpmsize) {
    const char *p = text;
    static const char level_key[] = "battery_level: ";
    if (strncmp(p, level_key, sizeof(level_key) - 1) != 0)
        return -1;
    p += sizeof(level_key) - 1;
    const char *nl = strchr(p, '\n');
    if (nl == NULL || !two_decimal_number(p, (size_t) (nl - p)))
        return -1;
    p = nl + 1;

    static const char state_key[] = "battery_state: ";
    if (strncmp(p, state_key, sizeof(state_key) - 1) != 0)
        return -1;
    p += sizeof(state_key) - 1;
    nl = strchr(p, '\n');
    if (nl == NULL || (size_t) (nl - p) >= statesize)
        return -1;
    memcpy(state, p, (size_t) (nl - p));
    state[nl - p] = '\0';
    if (!battery_state_word(state))
        return -1;
    p = nl + 1;

    static const char lpm_key[] = "low_power_mode: ";
    if (strncmp(p, lpm_key, sizeof(lpm_key) - 1) != 0)
        return -1;
    p += sizeof(lpm_key) - 1;
    nl = strchr(p, '\n');
    if (nl == NULL || (size_t) (nl - p) >= lpmsize)
        return -1;
    memcpy(lpm, p, (size_t) (nl - p));
    lpm[nl - p] = '\0';
    if (strcmp(lpm, "Enabled") != 0 && strcmp(lpm, "Disabled") != 0 && strcmp(lpm, "Unknown") != 0)
        return -1;
    return nl[1] == '\0' ? 0 : -1;
}

static void *reader(void *arg) {
    long bad = 0;
    (void) arg;
    for (int i = 0; i < READS_PER_READER; i++) {
        char text[256], state[32], lpm[32], capacity[64], status[64];
        if (slurp("/proc/ish/BAT0", text, sizeof(text)) <= 0 ||
                parse_bat0(text, state, sizeof(state), lpm, sizeof(lpm)) != 0)
            bad++;
        ssize_t n = slurp("/proc/ish/BAT0_capacity", capacity, sizeof(capacity));
        if (n < 2 || capacity[n - 1] != '\n' || !two_decimal_number(capacity, (size_t) n - 1))
            bad++;
        n = slurp("/proc/ish/BAT0_status", status, sizeof(status));
        if (n < 2 || status[n - 1] != '\n')
            bad++;
        else {
            status[n - 1] = '\0';
            if (!battery_state_word(status))
                bad++;
        }
    }
    return (void *) bad;
}

static void check_battery_files(int cli) {
    char text[256], state[32], lpm[32];
    ssize_t n = slurp("/proc/ish/BAT0", text, sizeof(text));
    if (!check("/proc/ish/BAT0 is readable", n > 0))
        return;
    test_logf("     /proc/ish/BAT0:\n%s", text);
    check("/proc/ish/BAT0 is battery_level, battery_state, low_power_mode, one per line",
          parse_bat0(text, state, sizeof(state), lpm, sizeof(lpm)) == 0);

    if (cli) {
        // Nothing to ask, so nothing claimed -- in the app's own format.
        check("CLI: BAT0 says the state is unknown, with the app's -100.00 level",
              strcmp(text, "battery_level: -100.00\nbattery_state: Unknown\nlow_power_mode: Unknown\n") == 0);
        char buf[64];
        check("CLI: BAT0_capacity is -100.00",
              slurp("/proc/ish/BAT0_capacity", buf, sizeof(buf)) > 0 && strcmp(buf, "-100.00\n") == 0);
        check("CLI: BAT0_status is Unknown",
              slurp("/proc/ish/BAT0_status", buf, sizeof(buf)) > 0 && strcmp(buf, "Unknown\n") == 0);
    }

    pthread_t threads[READERS];
    int started = 0;
    for (int i = 0; i < READERS; i++) {
        if (pthread_create(&threads[i], NULL, reader, NULL) != 0)
            break;
        started++;
    }
    check("start the concurrent readers", started == READERS);
    long bad = 0;
    for (int i = 0; i < started; i++) {
        void *result;
        pthread_join(threads[i], &result);
        bad += (long) result;
    }
    char label[128];
    snprintf(label, sizeof(label), "%d concurrent reads of BAT0, BAT0_capacity and BAT0_status all parse (%ld did not)",
             started * READS_PER_READER * 3, bad);
    check(label, bad == 0);
}

static void check_thermal_state(int cli) {
    char buf[64];
    ssize_t n = slurp("/proc/ish/thermal_state", buf, sizeof(buf));
    if (!check("/proc/ish/thermal_state is readable", n > 0))
        return;
    check("thermal_state is one line", buf[n - 1] == '\n' && strchr(buf, '\n') == buf + n - 1);
    buf[n - 1] = '\0';
    test_logf("     thermal_state: %s\n", buf);
    char label[200];
    snprintf(label, sizeof(label), "thermal_state is nominal, fair, serious, critical or unknown (got \"%s\")", buf);
    check(label, strcmp(buf, "nominal") == 0 || strcmp(buf, "fair") == 0 ||
                 strcmp(buf, "serious") == 0 || strcmp(buf, "critical") == 0 ||
                 strcmp(buf, "unknown") == 0);
    if (cli)
        check("CLI: thermal_state is unknown -- there is no source to ask", strcmp(buf, "unknown") == 0);
}

// Usable as a path under /usr/share/zoneinfo, and nothing else.
static int plausible_zone_name(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len >= 128 || name[0] == '/' || name[len - 1] == '/')
        return 0;
    for (const char *p = name; *p != '\0'; p++) {
        if (!isalnum((unsigned char) *p) && strchr("/_+-.", *p) == NULL)
            return 0;
    }
    const char *component = name;
    for (;;) {
        const char *slash = strchr(component, '/');
        size_t clen = slash != NULL ? (size_t) (slash - component) : strlen(component);
        if (clen == 0 || (clen == 1 && component[0] == '.') ||
                (clen == 2 && component[0] == '.' && component[1] == '.'))
            return 0;
        if (slash == NULL)
            return 1;
        component = slash + 1;
    }
}

static void check_timezone(int cli) {
    char buf[256];
    ssize_t n = slurp("/proc/ish/timezone", buf, sizeof(buf));
    if (!check("/proc/ish/timezone is readable", n > 0))
        return;
    check("timezone is one line", buf[n - 1] == '\n' && strchr(buf, '\n') == buf + n - 1);
    buf[n - 1] = '\0';
    test_logf("     timezone: \"%s\"\n", buf);
    if (buf[0] == '\0') {
        // Allowed -- a host with no zone to name -- but not on a Mac, where
        // /etc/localtime is always a link into the zoneinfo tree, and not in
        // the app, where the system always has a zone.
        check("timezone is only empty on a non-Darwin command-line host", cli && !host_is_darwin());
        return;
    }
    char label[300];
    snprintf(label, sizeof(label), "timezone is a plausible IANA name (got \"%s\")", buf);
    check(label, plausible_zone_name(buf));

    // Where the guest has the zone database, the host's name is normally in
    // it. Informational only: a guest's tzdata can be older than the host's.
    char path[400];
    snprintf(path, sizeof(path), "/usr/share/zoneinfo/%s", buf);
    if (access("/usr/share/zoneinfo", F_OK) == 0)
        test_logf("     %s %s\n", path, access(path, F_OK) == 0 ? "exists" : "is not in this root's tzdata");
}

// Every character of s is a decimal digit, and there is at least one.
static int all_digits(const char *s) {
    if (*s == '\0')
        return 0;
    for (; *s != '\0'; s++)
        if (!isdigit((unsigned char) *s))
            return 0;
    return 1;
}

// /proc/ish/applets: the Workspace applets, which are app UI and so appear in
// no process list. A fixed header, then "ID TOOL DESKTOP STATE AGE TITLE" per
// applet with the title last. The command-line build has no Workspace, so the
// header is the whole file there; on a device the rows are whatever happens to
// be open, so each one is checked for shape rather than content.
static void check_applets(int cli) {
    static char buf[16384];
    ssize_t n = slurp("/proc/ish/applets", buf, sizeof(buf));
    if (!check("/proc/ish/applets is readable", n > 0))
        return;
    static const char header[] = "ID TOOL DESKTOP STATE AGE TITLE\n";
    if (!check("applets starts with its header", strncmp(buf, header, strlen(header)) == 0))
        return;
    char *rows = buf + strlen(header);
    if (cli)
        check("applets lists nothing in the command-line build", *rows == '\0');
    int count = 0;
    char *save = NULL;
    for (char *line = strtok_r(rows, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        char id[32], tool[64], desktop[32], state[16], age[32];
        int title_at = -1;
        int ok = sscanf(line, "%31s %63s %31s %15s %31s %n", id, tool, desktop, state, age, &title_at) == 5 &&
                 title_at > 0 && line[title_at] != '\0' &&
                 all_digits(id) && all_digits(age) &&
                 (all_digits(desktop) || strcmp(desktop, "*") == 0) &&
                 (strcmp(state, "front") == 0 || strcmp(state, "shown") == 0 ||
                  strcmp(state, "hidden") == 0);
        test_logf("     applet: %s\n", line);
        if (!ok)
            printf("FAIL applets row does not parse: \"%s\"\n", line);
        failures_total += !ok;
        count++;
    }
    test_logf("     %d applet(s) open\n", count);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    if (access("/proc/ish", F_OK) != 0) {
        printf("proc_ish_host_state: SKIP (no /proc/ish -- not AOK)\n");
        return 0;
    }
    int cli = is_cli();
    test_logf("     %s build\n", cli ? "command-line" : "app");

    check_battery_files(cli);
    check_thermal_state(cli);
    check_timezone(cli);
    check_applets(cli);
    return finish_suite("proc_ish_host_state");
}
