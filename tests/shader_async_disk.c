// CPU fixture for runtime/gfx_shader.c background loading, bulk .use preload, and buffered nvn2-origins writes.
// Includes the production gfx_shader.c TU, links real platform threads/locks/files + sha256, intercepts
// plat_file_pread to record caller-thread reads, and mocks plat_child_* for translator fallback tests.
#define plat_file_pread fixture_file_pread
#define plat_child_start fixture_child_start
#define plat_child_stop fixture_child_stop
#define plat_child_write fixture_child_write
#define plat_child_read fixture_child_read
#include "../runtime/gfx_shader.c"
#include "sha256.h"
#undef plat_file_pread
#undef plat_child_start
#undef plat_child_stop
#undef plat_child_write
#undef plat_child_read

extern __typeof__(fixture_file_pread) plat_file_pread;
extern __typeof__(fixture_child_start) plat_child_start;
extern __typeof__(fixture_child_stop) plat_child_stop;
extern __typeof__(fixture_child_write) plat_child_write;
extern __typeof__(fixture_child_read) plat_child_read;

static uint64_t g_test_origin;
uint64_t nvn2_origin(const uint8_t *code) {
    (void)code;
    return g_test_origin;
}
int nvn2_lower(const uint8_t *control, const uint8_t *code, const uint8_t **out_control, const uint8_t **out_code) {
    (void)control;
    (void)code;
    (void)out_control;
    (void)out_code;
    return 0;
}
const char *cfg_get(const char *name) {   // the queued disk load is on unless the test sets DKB_SHADER_LOAD_QUEUE=0
    if (strcmp(name, "DKB_SHADER_LOAD_QUEUE")) return NULL;
    const char *v = getenv(name);
    return v && !strcmp(v, "0") ? "0" : "1";
}
void rt_log(const char *format, ...) {
    (void)format;
}
_Noreturn void rt_fatal(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}
void rt_sha256(const void *data, size_t size, uint8_t out[32]) {
    sha256(data, size, out);
}
int kern_core_away(void) { return 0; }
void kern_core_back(int *away) { (void)away; }
uint64_t perf_begin(void) { return 1; }
void perf_end(int kind, uint64_t start) {
    (void)kind;
    (void)start;
}

static volatile int64_t g_pread_calls;
static volatile int64_t g_pread_on_caller;
static uint32_t g_caller_tid;

int64_t fixture_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    __atomic_fetch_add(&g_pread_calls, 1, __ATOMIC_RELAXED);
    uint32_t target = __atomic_load_n(&g_caller_tid, __ATOMIC_RELAXED);
    if (target && plat_thread_id() == target) __atomic_fetch_add(&g_pread_on_caller, 1, __ATOMIC_RELAXED);
    return plat_file_pread(f, buf, n, off);
}

typedef struct {
    char req[8192];
    size_t req_len;
    int replied;
    size_t reply_pos;
} MockChild;

PlatChild *fixture_child_start(const char *exe, const char *arg, uint32_t *err) {
    (void)exe;
    (void)arg;
    (void)err;
    return (PlatChild *)calloc(1, sizeof(MockChild));
}

void fixture_child_stop(PlatChild *p) {
    free(p);
}

static void mock_produce_shader(const char *key_hex_str) {
    static const uint32_t spv[5] = {0x07230203u, 0x00010000u, 0u, 1u, 0u};
    static const char res[] = "{\"stage\":\"vs\"}";
    char path[PATH_CAP + 80];
    snprintf(path, sizeof path, "%s/%s.spv", cache_dir, key_hex_str);
    write_file(path, spv, sizeof spv);
    snprintf(path, sizeof path, "%s/%s.res.json", cache_dir, key_hex_str);
    write_file(path, res, sizeof res - 1);
}

int fixture_child_write(PlatChild *p, const void *d, size_t n) {
    MockChild *m = (MockChild *)p;
    if (!m || m->req_len + n >= sizeof m->req) return 0;
    memcpy(m->req + m->req_len, d, n);
    m->req_len += n;
    m->req[m->req_len] = 0;
    if (memchr(d, '\n', n)) {
        const char *k = strstr(m->req, "\"key\":\"");
        if (k) {
            k += 7;
            char hex[65] = {0};
            if (strlen(k) >= 64) {
                memcpy(hex, k, 64);
                mock_produce_shader(hex);
            }
        }
        m->replied = 1;
        m->reply_pos = 0;
        m->req_len = 0;
    }
    return 1;
}

long fixture_child_read(PlatChild *p, void *d, size_t n) {
    MockChild *m = (MockChild *)p;
    static const char ok[] = "ok 0\n";
    if (!m || !m->replied) return 0;
    size_t left = (sizeof ok - 1) - m->reply_pos;
    size_t take = left < n ? left : n;
    memcpy(d, ok + m->reply_pos, take);
    m->reply_pos += take;
    if (m->reply_pos == sizeof ok - 1) m->replied = 0;
    return (long)take;
}

static void setup(const char *dir) {
    if (strlen(dir) >= sizeof cache_dir) abort();
    strcpy(cache_dir, dir);
    paths_once.state = 2;
    for (unsigned i = 0; i < sizeof xlate_id; i++) xlate_id[i] = (uint8_t)(0x42 + i);
    for (unsigned i = 0; i < sizeof host_sg / sizeof *host_sg; i++) host_sg[i] = 32;
    snprintf(xlate_exe, sizeof xlate_exe, "%s/mock_xlate.exe", cache_dir);
    write_file(xlate_exe, "mock", 4);
    plat_file_mkdir_p(cache_dir);
}

static void hex_bytes(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
}

static void default_answers(GfxShaderAnswers *a) {
    memset(a, 0, sizeof *a);
    memset(a->tex_type, GFX_TEX_2D, sizeof a->tex_type);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    setup(argv[2]);

    if (!strcmp(argv[1], "test-preload")) {
        if (argc != 4) return 2;
        uint64_t n_hashes = 0;
        uint8_t *hashes = read_file(argv[3], &n_hashes);
        if (!hashes || n_hashes % 32) return 3;
        pack_open();
        __atomic_store_n(&g_pread_calls, 0, __ATOMIC_RELAXED);
        use_preload(NULL);
        int64_t calls = __atomic_load_n(&g_pread_calls, __ATOMIC_RELAXED);
        int found = 0;
        for (uint64_t off = 0; off < n_hashes; off += 32) {
            UseMask m = {0};
            if (use_find(hashes + off, &m) && m.slots == 3 && m.attrs == 5 && m.fouts == 1) found++;
        }
        printf("{\"pread_calls\":%lld,\"ready\":%d,\"found\":%d,\"expected\":%llu}\n",
               (long long)calls, (int)plat_cas32(&use_files_ready, 0, 0), found,
               (unsigned long long)(n_hashes / 32));
        free(hashes);
        return 0;
    }

    if (!strcmp(argv[1], "test-async-pack") || !strcmp(argv[1], "test-load-fallback")) {
        if (argc != 5) return 2;
        uint64_t cn = 0, nn = 0;
        uint8_t *code = read_file(argv[3], &cn);
        uint8_t *control = read_file(argv[4], &nn);
        if (!code || !control || nn != CONTROL_SIZE) return 3;
        pack_open();
        use_preload(NULL);
        GfxShaderAnswers a;
        default_answers(&a);
        __atomic_store_n(&g_caller_tid, plat_thread_id(), __ATOMIC_RELAXED);
        __atomic_store_n(&g_pread_on_caller, 0, __ATOMIC_RELAXED);
        int pending1 = -1, pending2 = -1;
        const GfxShader *s1 = gfx_shader_get_async(GFX_STAGE_VS, control, code, cn, &a, &pending1);
        int64_t caller_preads = __atomic_load_n(&g_pread_on_caller, __ATOMIC_RELAXED);
        const GfxShader *s_wait = gfx_shader_await(GFX_STAGE_VS, control, code, cn, &a);
        const GfxShader *s2 = gfx_shader_get_async(GFX_STAGE_VS, control, code, cn, &a, &pending2);
        printf("{\"pending1\":%d,\"s1_null\":%d,\"caller_preads\":%lld,\"await_ok\":%d,"
               "\"pending2\":%d,\"same_ptr\":%d,\"xlated\":%lld,\"hits\":%lld}\n",
               pending1, s1 == NULL, (long long)caller_preads, s_wait != NULL, pending2,
               s2 == s_wait && s_wait != NULL, (long long)n_xlated, (long long)n_hits);
        free(code);
        free(control);
        return 0;
    }

    if (!strcmp(argv[1], "test-origin-buffer")) {
        if (argc != 6) return 2;
        uint64_t cn = 0, nn = 0;
        uint8_t *code = read_file(argv[3], &cn);
        uint8_t *control = read_file(argv[4], &nn);
        uint64_t orig_val = strtoull(argv[5], NULL, 0);
        if (!code || !control || nn != CONTROL_SIZE) return 3;
        Req r;
        GfxShaderAnswers a;
        default_answers(&a);
        if (!req_init(&r, GFX_STAGE_VS, control, code, cn, NULL, &a)) return 4;
        GfxShader *s = calloc(1, sizeof *s);
        memcpy(s->key, r.key, 32);
        memcpy(s->iid, r.iid, 32);
        g_test_origin = orig_val;
        note_origin(s, code);
        char orig_path[PATH_CAP + 40];
        snprintf(orig_path, sizeof orig_path, "%s/nvn2-origins.bin", cache_dir);
        int64_t size_before_flush = plat_file_size(orig_path);
        gfx_shader_record_flush(1);
        int64_t size_after_flush = plat_file_size(orig_path);
        // Repeat note_origin with the same iid via a fresh GfxShader struct: should be deduplicated by origin_tab.
        GfxShader *s2 = calloc(1, sizeof *s2);
        memcpy(s2->key, r.key, 32);
        memcpy(s2->iid, r.iid, 32);
        note_origin(s2, code);
        gfx_shader_record_flush(1);
        int64_t size_after_dup = plat_file_size(orig_path);
        printf("{\"iid\":\"");
        hex_bytes(r.iid, 32);
        printf("\",\"before\":%lld,\"after\":%lld,\"after_dup\":%lld}\n",
               (long long)size_before_flush, (long long)size_after_flush, (long long)size_after_dup);
        free(s);
        free(s2);
        free(code);
        free(control);
        return 0;
    }

    if (!strcmp(argv[1], "inspect-req")) {
        if (argc != 5) return 2;
        uint64_t cn = 0, nn = 0;
        uint8_t *code = read_file(argv[3], &cn);
        uint8_t *control = read_file(argv[4], &nn);
        if (!code || !control || nn != CONTROL_SIZE) return 3;
        UseMask m = {.slots = 3, .attrs = 5, .fouts = 1};
        Req r;
        GfxShaderAnswers a;
        default_answers(&a);
        req_init(&r, GFX_STAGE_VS, control, code, cn, NULL, &a);
        use_store(r.code_hash, &m);
        req_init(&r, GFX_STAGE_VS, control, code, cn, NULL, &a);
        uint8_t uname[32];
        use_name(r.code_hash, uname);
        printf("{\"code_hash\":\"");
        hex_bytes(r.code_hash, 32);
        printf("\",\"use_name\":\"");
        hex_bytes(uname, 32);
        printf("\",\"key\":\"");
        hex_bytes(r.key, 32);
        printf("\",\"iid\":\"");
        hex_bytes(r.iid, 32);
        puts("\"}");
        free(code);
        free(control);
        return 0;
    }

    return 2;
}
