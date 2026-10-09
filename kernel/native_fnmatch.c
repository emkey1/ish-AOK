// fnmatch(3) for natively-compiled programs (kernel/native_libc.h renames
// their calls to nlibc_fnmatch): glibc's rules, not Darwin's.
//
// A native program is compiled as host code, so it called Darwin's fnmatch,
// and the two libcs disagree about patterns a Linux user writes every day.
// Darwin refuses outright (an error, 2) a pattern with a `[` that opens no
// bracket expression -- `find -name 'a[b'`, `ls --ignore='x['` -- and one
// ending in a lone `\`, where glibc matches the `[` as itself; it reads `[]`,
// `[[:bogus:]]` and an unclosed class differently too. Over 55 patterns, 34
// names and 8 flag sets, 2,340 of 14,960 answers differed (glibc on camd,
// Linux 6.12). SmallCLUE's find, grep, ls, du, diff, tar and git, and scp,
// are the callers; dash has its own matcher (kernel/dash_config_aok.h).
//
// This follows glibc's posix/fnmatch_loop.c for the C locale's collation
// (no named collating elements; ranges by code point) and agrees with it on
// every case of that table. As glibc does, a multibyte locale (MB_CUR_MAX >
// 1) matches characters, not bytes, and a pattern or name that is not valid
// in it is an error (-1). FNM_EXTMATCH is glibc's alone and not here: the
// host's <fnmatch.h> has no such flag, so no caller can ask for it.
#include <ctype.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "kernel/native_libc.h"

#define CHAR_CLASS_MAX_LENGTH 256

struct fnm {
    int flags;
    bool wide;                  // characters are code points, not bytes
};

static uint32_t fnm_fold(const struct fnm *m, uint32_t c) {
    if (!(m->flags & FNM_CASEFOLD))
        return c;
    if (m->wide)
        return (uint32_t) towlower((wint_t) c);
    return c < 0x80 ? (uint32_t) tolower((int) c) : c;
}

static bool fnm_class(const struct fnm *m, const char *name, uint32_t c, bool *valid) {
    wctype_t wt = wctype(name);
    *valid = wt != 0;
    if (wt == 0)
        return false;
    // a byte past ASCII is no character of the C locale's classes
    if (!m->wide && c >= 0x80)
        return false;
    return iswctype((wint_t) c, wt) != 0;
}

// The leading-period rule: a `.` at the start, or after a `/` with
// FNM_PATHNAME, must be matched by a `.` in the pattern.
#define NO_LEADING_PERIOD(flags) \
    (((flags) & (FNM_PATHNAME | FNM_PERIOD)) == (FNM_PATHNAME | FNM_PERIOD))

static int fnm_match(const struct fnm *m, const uint32_t *p, const uint32_t *n,
                     const uint32_t *string_end, bool no_leading_period, int flags) {
    uint32_t c;
    while ((c = *p++) != 0) {
        bool new_no_leading_period = false;
        c = fnm_fold(m, c);
        switch (c) {
        case '?':
            if (n == string_end)
                return FNM_NOMATCH;
            if (*n == '/' && (flags & FNM_PATHNAME))
                return FNM_NOMATCH;
            if (*n == '.' && no_leading_period)
                return FNM_NOMATCH;
            break;

        case '\\':
            if (!(flags & FNM_NOESCAPE)) {
                c = *p++;
                if (c == 0)
                    return FNM_NOMATCH;     // a trailing `\` loses
                c = fnm_fold(m, c);
            }
            if (n == string_end || fnm_fold(m, *n) != c)
                return FNM_NOMATCH;
            break;

        case '*': {
            if (n != string_end && *n == '.' && no_leading_period)
                return FNM_NOMATCH;
            for (c = *p++; c == '?' || c == '*'; c = *p++) {
                if (c == '?') {
                    if (n == string_end)
                        return FNM_NOMATCH;
                    if (*n == '/' && (flags & FNM_PATHNAME))
                        return FNM_NOMATCH;
                    ++n;
                }
            }
            if (c == 0) {
                // The wildcards end the pattern: with FNM_PATHNAME, a `/`
                // left in the name means no match, unless FNM_LEADING_DIR.
                if (!(flags & FNM_PATHNAME))
                    return 0;
                if (flags & FNM_LEADING_DIR)
                    return 0;
                for (const uint32_t *q = n; q < string_end; q++)
                    if (*q == '/')
                        return FNM_NOMATCH;
                return 0;
            }
            const uint32_t *endp = string_end;
            if (flags & FNM_PATHNAME) {
                for (const uint32_t *q = n; q < string_end; q++)
                    if (*q == '/') {
                        endp = q;
                        break;
                    }
            }
            if (c == '[') {
                int flags2 = (flags & FNM_PATHNAME) ? flags : (flags & ~FNM_PERIOD);
                bool nlp2 = no_leading_period;
                for (--p; n < endp; ++n, nlp2 = false)
                    if (fnm_match(m, p, n, string_end, nlp2, flags2) == 0)
                        return 0;
            } else if (c == '/' && (flags & FNM_PATHNAME)) {
                while (n < string_end && *n != '/')
                    ++n;
                if (n < string_end && *n == '/' &&
                        fnm_match(m, p, n + 1, string_end, (flags & FNM_PERIOD) != 0, flags) == 0)
                    return 0;
            } else {
                int flags2 = (flags & FNM_PATHNAME) ? flags : (flags & ~FNM_PERIOD);
                if (c == '\\' && !(flags & FNM_NOESCAPE))
                    c = *p;
                c = fnm_fold(m, c);
                for (--p; n < endp; ++n)
                    if (fnm_fold(m, *n) == c && fnm_match(m, p, n, string_end, false, flags2) == 0)
                        return 0;
            }
            return FNM_NOMATCH;
        }

        case '[': {
            const uint32_t *p_init = p;
            const uint32_t *n_init = n;
            if (n == string_end)
                return FNM_NOMATCH;
            if (*n == '.' && no_leading_period)
                return FNM_NOMATCH;
            if (*n == '/' && (flags & FNM_PATHNAME))
                return FNM_NOMATCH;     // a bracket never matches `/` then
            bool not = *p == '!' || *p == '^';
            if (not)
                ++p;
            uint32_t fn = fnm_fold(m, *n);
            uint32_t cold;

            c = *p++;
            for (;;) {
                bool is_range;
                if (!(flags & FNM_NOESCAPE) && c == '\\') {
                    if (*p == 0)
                        return FNM_NOMATCH;
                    c = fnm_fold(m, *p);
                    ++p;
                    goto normal_bracket;
                } else if (c == '[' && *p == ':') {
                    char str[CHAR_CLASS_MAX_LENGTH + 1];
                    size_t c1 = 0;
                    const uint32_t *startp = p;
                    for (;;) {
                        if (c1 == CHAR_CLASS_MAX_LENGTH)
                            return FNM_NOMATCH;     // too long: ill-formed
                        c = *++p;
                        if (c == ':' && p[1] == ']') {
                            p += 2;
                            break;
                        }
                        if (c < 'a' || c >= 'z') {
                            // no class name can be this: a plain `[`
                            p = startp;
                            c = '[';
                            goto normal_bracket;
                        }
                        str[c1++] = (char) c;
                    }
                    str[c1] = '\0';
                    bool valid;
                    bool in = fnm_class(m, str, *n, &valid);
                    if (!valid)
                        return FNM_NOMATCH;         // no such class
                    if (in)
                        goto matched;
                    c = *p++;
                } else if (c == '[' && *p == '=') {
                    const uint32_t *startp = p;
                    c = *++p;
                    if (c == 0) {
                        p = startp;
                        c = '[';
                        goto normal_bracket;
                    }
                    uint32_t str = c;
                    c = *++p;
                    if (c != '=' || p[1] != ']') {
                        p = startp;
                        c = '[';
                        goto normal_bracket;
                    }
                    p += 2;
                    if (*n == str)
                        goto matched;
                    c = *p++;
                } else if (c == 0) {
                    // an unterminated `[` is an ordinary character
                    p = p_init;
                    n = n_init;
                    c = '[';
                    goto normal_match;
                } else {
                    if (c == '[' && *p == '.') {
                        const uint32_t *startp = p;
                        size_t c1 = 0;
                        for (;;) {
                            c = *++p;
                            if (c == '.' && p[1] == ']') {
                                p += 2;
                                break;
                            }
                            if (c == 0)
                                return FNM_NOMATCH;
                            ++c1;
                        }
                        is_range = *p == '-' && p[1] != 0;
                        // the C locale names no collating elements: only
                        // the character itself, [.a.]
                        if (c1 != 1)
                            return FNM_NOMATCH;
                        if (!is_range && *n == startp[1])
                            goto matched;
                        cold = startp[1];
                        c = *p++;
                    } else {
                        c = fnm_fold(m, c);
                    normal_bracket:
                        is_range = *p == '-' && p[1] != 0 && p[1] != ']';
                        if (!is_range && c == fn)
                            goto matched;
                        cold = c;
                        c = *p++;
                    }
                    if (c == '-' && *p != ']') {
                        uint32_t cend = *p++;
                        if (!(flags & FNM_NOESCAPE) && cend == '\\')
                            cend = *p++;
                        if (cend == 0)
                            return FNM_NOMATCH;
                        if (cend == '[' && *p == '.') {
                            // a collating symbol ends the range
                            const uint32_t *startp = p;
                            size_t c1 = 0;
                            for (;;) {
                                uint32_t d = *++p;
                                if (d == '.' && p[1] == ']') {
                                    p += 2;
                                    break;
                                }
                                if (d == 0)
                                    return FNM_NOMATCH;
                                ++c1;
                            }
                            if (c1 != 1)
                                return FNM_NOMATCH;
                            cend = startp[1];
                        }
                        cend = fnm_fold(m, cend);
                        if (cold <= fn && fn <= cend)
                            goto matched;
                        c = *p++;
                    }
                }
                if (c == ']')
                    break;
            }
            if (!not)
                return FNM_NOMATCH;
            break;

        matched:
            // skip the rest of a bracket expression that matched
            for (;;) {
                c = *p++;
                if (c == 0)
                    return FNM_NOMATCH;     // [... unterminated loses
                if (!(flags & FNM_NOESCAPE) && c == '\\') {
                    if (*p == 0)
                        return FNM_NOMATCH;
                    ++p;
                } else if (c == '[' && *p == ':') {
                    int c1 = 0;
                    const uint32_t *startp = p;
                    bool plain = false;
                    for (;;) {
                        c = *++p;
                        if (++c1 == CHAR_CLASS_MAX_LENGTH)
                            return FNM_NOMATCH;
                        if (*p == ':' && p[1] == ']')
                            break;
                        if (c < 'a' || c >= 'z') {
                            p = startp;
                            plain = true;
                            break;
                        }
                    }
                    if (plain)
                        continue;
                    p += 2;
                } else if (c == '[' && *p == '=') {
                    c = *++p;
                    if (c == 0)
                        return FNM_NOMATCH;
                    c = *++p;
                    if (c != '=' || p[1] != ']')
                        return FNM_NOMATCH;
                    p += 2;
                } else if (c == '[' && *p == '.') {
                    for (;;) {
                        c = *++p;
                        if (c == 0)
                            return FNM_NOMATCH;
                        if (c == '.' && p[1] == ']')
                            break;
                    }
                    p += 2;
                }
                if (c == ']')
                    break;
            }
            if (not)
                return FNM_NOMATCH;
            break;

        normal_match:
            if (n == string_end || c != fnm_fold(m, *n))
                return FNM_NOMATCH;
            break;
        }

        case '/':
            if (NO_LEADING_PERIOD(flags)) {
                if (n == string_end || c != *n)
                    return FNM_NOMATCH;
                new_no_leading_period = true;
                break;
            }
            __attribute__((fallthrough));
        default:
            if (n == string_end || c != fnm_fold(m, *n))
                return FNM_NOMATCH;
        }
        no_leading_period = new_no_leading_period;
        ++n;
    }
    if (n == string_end)
        return 0;
    // "foo*" matches "foobar/frobozz"
    if ((flags & FNM_LEADING_DIR) && *n == '/')
        return 0;
    return FNM_NOMATCH;
}

// Into code points (or bytes as they are), NUL-terminated; NULL on an invalid
// sequence, with *bad set, or on no memory.
static uint32_t *fnm_decode(const char *s, bool wide, size_t *len, bool *bad) {
    size_t n = strlen(s);
    uint32_t *out = malloc((n + 1) * sizeof(*out));
    if (out == NULL)
        return NULL;
    size_t k = 0;
    if (!wide) {
        for (size_t i = 0; i < n; i++)
            out[k++] = (unsigned char) s[i];
    } else {
        mbstate_t st;
        memset(&st, 0, sizeof(st));
        size_t i = 0;
        while (i < n) {
            wchar_t wc;
            size_t r = mbrtowc(&wc, s + i, n - i, &st);
            if (r == (size_t) -1 || r == (size_t) -2) {
                free(out);
                *bad = true;
                return NULL;
            }
            if (r == 0)
                r = 1;
            out[k++] = (uint32_t) wc;
            i += r;
        }
    }
    out[k] = 0;
    *len = k;
    return out;
}

static int fnm_run(const char *pattern, const char *string, int flags, bool wide, bool *bad) {
    struct fnm m = {.flags = flags, .wide = wide};
    size_t plen, slen;
    uint32_t *p = fnm_decode(pattern, wide, &plen, bad);
    if (p == NULL)
        return -1;
    uint32_t *s = fnm_decode(string, wide, &slen, bad);
    if (s == NULL) {
        free(p);
        return -1;
    }
    int r = fnm_match(&m, p, s, s + slen, (flags & FNM_PERIOD) != 0, flags);
    free(p);
    free(s);
    return r;
}

int nlibc_fnmatch(const char *pattern, const char *string, int flags) {
    if (MB_CUR_MAX > 1) {
        bool bad = false;
        int r = fnm_run(pattern, string, flags, true, &bad);
        if (r == 0)
            return 0;
        if (r != FNM_NOMATCH && !bad)
            return r;   // no memory
        // Then as bytes: glibc 2.41 matches when either way does, and an
        // invalid sequence is matched as bytes rather than refused -- `??`
        // matches the two-byte "é" as `?` does, and `?` a lone 0xff.
    }
    bool bad = false;
    return fnm_run(pattern, string, flags, false, &bad);
}
