#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef struct JSWeakRefRecord JSWeakRefRecord;
struct BaselineString {
    uint32_t len : 31;
    uint32_t is_wide_char : 1; /* 0 = 8 bits, 1 = 16 bits characters */
    /* for JS_ATOM_TYPE_SYMBOL: hash = 0, atom_type = 3,
       for JS_ATOM_TYPE_PRIVATE: hash = 1, atom_type = 3
       XXX: could change encoding to have one more bit in hash */
    uint32_t hash : 28;
    uint32_t kind : 2;
    uint32_t atom_type : 2; /* != 0 if atom, JS_ATOM_TYPE_x */
    uint32_t hash_next;     /* atom_index for JS_ATOM_TYPE_SYMBOL */
    JSWeakRefRecord *first_weak_ref;
#ifdef ENABLE_DUMPS        // JS_DUMP_LEAKS
    struct list_head link; /* string list */
#endif
};

struct CachedString {
    uint32_t len : 31;
    uint32_t is_wide_char : 1; /* 0 = 8 bits, 1 = 16 bits characters */
    /* for JS_ATOM_TYPE_SYMBOL: hash = 0, atom_type = 3,
       for JS_ATOM_TYPE_PRIVATE: hash = 1, atom_type = 3
       XXX: could change encoding to have one more bit in hash */
    uint32_t hash : 28;
    uint32_t kind : 2;
    uint32_t atom_type : 2; /* != 0 if atom, JS_ATOM_TYPE_x */
    uint32_t hash_next;     /* atom_index for JS_ATOM_TYPE_SYMBOL */
    uint32_t content_hash;
    JSWeakRefRecord *first_weak_ref;
#ifdef ENABLE_DUMPS        // JS_DUMP_LEAKS
    struct list_head link; /* string list */
#endif
};

int main(void) {
    printf("baseline_size=%zu candidate_size=%zu hash_next_offset=%zu weak_offset=%zu candidate_cache_offset=%zu "
           "candidate_weak_offset=%zu\n",
           sizeof(struct BaselineString),
           sizeof(struct CachedString),
           offsetof(struct BaselineString, hash_next),
           offsetof(struct BaselineString, first_weak_ref),
           offsetof(struct CachedString, content_hash),
           offsetof(struct CachedString, first_weak_ref));
}
