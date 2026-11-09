from pathlib import Path
import hashlib

def once(src,old,new):
 assert src.count(old)==1, (old,src.count(old))
 return src.replace(old,new)

def transform(src):
 src=once(src,'    uint32_t hash_next; /* atom_index for JS_ATOM_TYPE_SYMBOL */','    uint32_t hash_next; /* atom_index for JS_ATOM_TYPE_SYMBOL */\n    uint32_t string_lifecycle_seen;')
 src=once(src,'    str->hash_next = 0;     /* optional */','    str->hash_next = 0;     /* optional */\n    str->string_lifecycle_seen = 0;')
 src=once(src,'            p->kind = JS_STRING_KIND_NORMAL;','            p->kind = JS_STRING_KIND_NORMAL;\n            p->string_lifecycle_seen = 0;')
 src=once(src,'        p->kind = JS_STRING_KIND_NORMAL;\n#ifdef ENABLE_DUMPS','        p->kind = JS_STRING_KIND_NORMAL;\n        p->string_lifecycle_seen = 0;\n#ifdef ENABLE_DUMPS')
 helper='''typedef struct {
    unsigned long long calls, repeat, units, bytes, repeat_bytes;
} StringLifecycleStats;
static StringLifecycleStats string_lifecycle_map[12], string_lifecycle_hash[4][12];
static unsigned long long string_lifecycle_rope_map, string_lifecycle_rope_units;
static unsigned long long string_lifecycle_flatten[2], string_lifecycle_flatten_units[2];
static unsigned long long string_lifecycle_append, string_lifecycle_append_seen;
static bool string_lifecycle_started;
static void string_lifecycle_report(void)
{
    fprintf(stderr, "STRING_LIFECYCLE layout size=%zu cache_offset=%zu weak_offset=%zu slice_size=%zu\\n",
            sizeof(JSString), offsetof(JSString, string_lifecycle_seen), offsetof(JSString, first_weak_ref), sizeof(JSStringSlice));
    for (int k = 0; k < 12; k++) {
        StringLifecycleStats *s = &string_lifecycle_map[k];
        fprintf(stderr, "STRING_LIFECYCLE map kind=%d wide=%d atom=%d calls=%llu repeat=%llu units=%llu bytes=%llu repeat_bytes=%llu\\n",
            k/4, (k/2)%2, k%2, s->calls, s->repeat, s->units, s->bytes, s->repeat_bytes);
        for (int seed = 0; seed < 4; seed++) {
            s = &string_lifecycle_hash[seed][k];
            fprintf(stderr, "STRING_LIFECYCLE hash seed=%d kind=%d wide=%d atom=%d calls=%llu repeat=%llu units=%llu bytes=%llu repeat_bytes=%llu\\n",
                seed, k/4, (k/2)%2, k%2, s->calls, s->repeat, s->units, s->bytes, s->repeat_bytes);
        }
    }
    fprintf(stderr, "STRING_LIFECYCLE rope_map=%llu rope_units=%llu flatten_new=%llu flatten_new_units=%llu flatten_reuse=%llu flatten_reuse_units=%llu append=%llu append_seen=%llu\\n",
        string_lifecycle_rope_map, string_lifecycle_rope_units,
        string_lifecycle_flatten[0], string_lifecycle_flatten_units[0],
        string_lifecycle_flatten[1], string_lifecycle_flatten_units[1],
        string_lifecycle_append, string_lifecycle_append_seen);
}
static void string_lifecycle_record(JSString *str, uint32_t seed, bool map)
{
    unsigned k = str->kind * 4 + str->is_wide_char * 2 + !!str->atom_type;
    uint32_t bit = map ? 1 : seed < 3 ? 2U << seed : 0;
    StringLifecycleStats *s = map ? &string_lifecycle_map[k] : &string_lifecycle_hash[seed < 3 ? seed : 3][k];
    unsigned long long bytes = (unsigned long long)str->len << str->is_wide_char;
    if (!string_lifecycle_started) { string_lifecycle_started = true; atexit(string_lifecycle_report); }
    s->calls++; s->units += str->len; s->bytes += bytes;
    if (str->string_lifecycle_seen & bit) { s->repeat++; s->repeat_bytes += bytes; }
    str->string_lifecycle_seen |= bit;
}

'''
 src=once(src,'static uint32_t hash_string(JSString *str, uint32_t h)\n{',helper+'static uint32_t hash_string(JSString *str, uint32_t h)\n{\n    string_lifecycle_record(str, h, false);')
 src=once(src,'        h = hash_string(JS_VALUE_GET_STRING(key), 0);','        string_lifecycle_record(JS_VALUE_GET_STRING(key), 0, true);\n        h = hash_string(JS_VALUE_GET_STRING(key), 0);')
 src=once(src,'        h = hash_string_rope(key, 0);','        string_lifecycle_rope_map++;\n        string_lifecycle_rope_units += JS_VALUE_GET_STRING_ROPE(key)->len;\n        h = hash_string_rope(key, 0);')
 src=once(src,'        /* Concatenate in place in available space at the end of p1 */','        /* Concatenate in place in available space at the end of p1 */\n        string_lifecycle_append++;\n        string_lifecycle_append_seen += !!p1->string_lifecycle_seen;\n        p1->string_lifecycle_seen = 0;')
 src=once(src,'        ret = js_dup(r->left);\n        return ret;','        string_lifecycle_flatten[1]++;\n        string_lifecycle_flatten_units[1] += r->len;\n        ret = js_dup(r->left);\n        return ret;')
 src=once(src,'    if (string_buffer_init2(ctx, b, r->len, r->is_wide_char))','    string_lifecycle_flatten[0]++;\n    string_lifecycle_flatten_units[0] += r->len;\n    if (string_buffer_init2(ctx, b, r->len, r->is_wide_char))')
 return src

if __name__=='__main__':
 root=Path(__file__).resolve().parents[2]/'build/lto-benchmark/string-lifecycle-analysis'
 out=root/'string-lifecycle-diagnostic';out.mkdir(parents=True, exist_ok=True)
 source = (root.parent/'integrated-regexp-anchor/quickjs-patched/quickjs.c').read_text()
 assert hashlib.sha256(source.encode()).hexdigest() == '4025d0bc28c45b8a68aaccd603b0309cfe53fef70dc37f3e3957f133fdf29d58', 'baseline source changed'
 (out/'quickjs.c').write_text(transform(source))
