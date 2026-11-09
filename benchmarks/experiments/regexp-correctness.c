#include "libregexp.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int baseline_exec(uint8_t **, const uint8_t *, const uint8_t *, int, int, int, void *);
static unsigned timeout_calls, timeout_limit, cases;
bool lre_check_stack_overflow(void *opaque, size_t n) {
    return false;
}
int lre_check_timeout(void *opaque) {
    return ++timeout_calls >= timeout_limit;
}
void *lre_realloc(void *opaque, void *ptr, size_t n) {
    if (!n) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, n);
}
static void run(const char *pattern, int flags, const void *text, int n, int wide, int start, int same_polls) {
    char err[256];
    int len;
    uint8_t *bc = lre_compile(&len, err, sizeof(err), pattern, strlen(pattern), flags, NULL);
    if (!bc) {
        return;
    }
    int alloc = lre_get_alloc_count(bc), count = lre_get_capture_count(bc) * 2;
    uint8_t **a = calloc(alloc, sizeof(*a)), **b = calloc(alloc, sizeof(*b));
    timeout_calls = 0;
    int ra = baseline_exec(a, bc, text, start, n, wide, NULL);
    unsigned pa = timeout_calls;
    timeout_calls = 0;
    int rb = lre_exec(b, bc, text, start, n, wide, NULL);
    unsigned pb = timeout_calls;
    if (ra != rb || (same_polls && pa != pb) || memcmp(a, b, count * sizeof(*a))) {
        fprintf(stderr,
                "FAIL pattern=%s flags=%d n=%d wide=%d start=%d ret=%d/%d polls=%u/%u\n",
                pattern,
                flags,
                n,
                wide,
                start,
                ra,
                rb,
                pa,
                pb);
        abort();
    }
    cases++;
    free(a);
    free(b);
    free(bc);
}
int main(void) {
    const char *patterns[] = { "^",           "^a",
                               "^(a)",        "(^a)",
                               "((^a)b)",     "^(a|b)$",
                               "^a|b",        "(^a)|b",
                               "(^)?a",       "(?=^a)a",
                               "(?<=^a)b",    "(?<!a)^b",
                               "a",           "ab",
                               "(a)b",        "((a))b",
                               "a*",          "a?",
                               "a|b",         "(a|b)c",
                               "a(b)?c",      "a(b|c)*d",
                               "a(?=b)",      "a(?!b)",
                               "a(?<=a)b",    "a(b)\\1",
                               "^(a+)\\1$",   "a$",
                               "[ab]c",       "\\ba",
                               "\\u{1f600}x", "\\ud800x",
                               "\\udc00x",    "éx",
                               "^é",          "^\\u{1f600}",
                               "a(?<x>b)?c",  "a(?:(b)|c)+d",
                               "a(?:b|c)*?d", "[]",
                               "[^]",         "(?:)",
                               "(())",        "\\p{Letter}",
                               "[a&&a]",      "a[\\q{ab|cd}]",
                               "a{2,4}" };
    const int flags[] = { 0, 1, 2, 4, 8, 16, 32, 64, 256, 17, 33, 20, 272, 258 };
    uint16_t units[] = { 0, 'a', 'b', 'c', 'd', 'x', '\n', '\r', 0xe9, 0x100, 0xd800, 0xdc00, 0xd83d, 0xde00, 0x2028 };
    uint32_t rng = 0x573812;
    timeout_limit = UINT32_MAX;
    for (unsigned p = 0; p < sizeof(patterns) / sizeof(*patterns); p++) {
        for (unsigned f = 0; f < sizeof(flags) / sizeof(*flags); f++) {
            for (int trial = 0; trial < 100; trial++) {
                uint8_t narrow[40];
                uint16_t wide[40];
                int n = trial % 40;
                for (int j = 0; j < n; j++) {
                    rng = rng * 1664525 + 1013904223;
                    wide[j] = units[(rng >> 16) % (sizeof(units) / sizeof(*units))];
                    narrow[j] = (uint8_t) wide[j];
                }
                for (int start = 0; start <= n; start++) {
                    run(patterns[p], flags[f], narrow, n, 0, start, 0);
                    run(patterns[p], flags[f], wide, n, 1, start, 0);
                }
            }
        }
    }
    int n = 200000;
    uint8_t *large = malloc(n);
    memset(large, 'x', n);
    const char *scan[] = { "z", "(z)", "z(a|b)", "x*y", "(?=z)z" };
    for (unsigned p = 0; p < sizeof(scan) / sizeof(*scan); p++) {
        /* Avoid quadratic catastrophic patterns in non-timeout runs. */
        timeout_limit = 1;
        run(scan[p], 0, large, n, 0, 0, 1);
        timeout_limit = 3;
        run(scan[p], 0, large, n, 0, 0, 1);
    }
    timeout_limit = UINT32_MAX;
    run("^z", 0, large, n, 0, 0, 0);
    run("^z", 0, large, n, 0, 1, 0);
    free(large);
    printf("PASS %u differential cases; literal/fallback interrupt callback parity verified\n", cases);
}
