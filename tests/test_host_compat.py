"""Exercise selected host hooks without game modules or a Darwin kernel."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / 'nxnative/dm/host'


def function(src, signature):
    start = src.index(signature)
    return src[start:src.index('\n}', start) + 3]


class HostCompatibility(unittest.TestCase):
    def run_c(self, code):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a host C compiler is required')
        with tempfile.TemporaryDirectory(prefix='nx2a-host-compat-') as temp:
            c = Path(temp) / 'check.c'
            exe = Path(temp) / 'check'
            c.write_text(code, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                                     '-pthread', str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_applet_focus_notifications_nesting_and_window_lifecycle(self):
        src = (HOST / 'hle_oe.c').read_text(encoding='utf-8')
        queue = src[src.index('enum { MSG_FOCUS'):src.index('void dm_set_opmode(')]
        focus = src[src.index('static _Atomic int g_focus ='):src.index('static int opmode(void)')]
        h = (HOST / 'hle.h').read_text(encoding='utf-8')
        applet = h[h.index('void dm_applet_enter('):h.index('// Audio pause')]
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#define LOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define die(...) abort()
static uint64_t now_ns(void) { return 123; }
static void hle_signal_system_event(void *p) { (void)p; }
static void hle_clear_system_event(void *p) { (void)p; }
''' + queue + focus + applet + r'''
static void drain(void) {
    assert(g_mqn <= 1);
    if (g_mqn) { assert(g_mq[g_mqh] == 15); g_mqh = (g_mqh+1)%MQ; g_mqn--; }
}
static int early_return(void) { DM_APPLET("keyboard"); assert(g_seen==2 && g_applets==1); return 7; }
int main(void) {
    (void)g_ntf_opmode; (void)g_ntf_perfmode;
    for (int mode=-1; mode<4; ++mode) {
        g_focus_mode=mode; int notify=mode!=0 && mode!=3;
        dm_applet_enter("controller");
        assert(g_seen==2 && dm_focus_now()==1 && g_mqn==notify); drain();
        dm_applet_enter("nested"); dm_applet_leave(); assert(g_seen==2 && !g_mqn);
        dm_focus_set(2); dm_focus_set(1); assert(g_seen==2 && !g_mqn);
        dm_applet_leave(); assert(g_seen==1 && g_mqn==notify); drain();
        dm_applet_enter("keyboard"); drain();
        int background=mode==1 ? 2 : 3;  // notify-only games remain OutOfFocus behind HOME
        dm_focus_set(3); assert(g_seen==background && dm_focus_now()==background && g_mqn==(mode==1 ? 0 : notify)); drain();
        dm_applet_leave(); assert(g_seen==background && !g_mqn);
        dm_focus_set(1); assert(g_seen==1 && g_mqn==notify); drain();
        // Android goes through OutOfFocus on HOME, then restores window input focus.
        dm_focus_set(2); assert(g_seen==2 && g_mqn==notify); drain();
        dm_focus_set(3); assert(g_seen==background && g_mqn==(mode==1 ? 0 : notify)); drain();
        dm_focus_set(2); assert(g_seen==2 && g_mqn==(mode==1 ? 0 : notify)); drain();
        dm_focus_set(1); assert(g_seen==1 && g_mqn==notify); drain();
        assert(early_return()==7 && !g_applets && g_seen==1 && g_mqn==notify); drain();
        dm_applet_enter("controller"); drain();
        dm_focus_set(2); dm_applet_leave(); assert(g_seen==2 && !g_mqn);
        dm_focus_set(1); assert(g_seen==1 && g_mqn==notify); drain();
    }
    assert(dm_focus_in_since()==123); return 0;
}
'''
        self.run_c(code)

    def test_controller_applet_cancels_unreachable_minimum_for_both_overloads(self):
        src = (HOST / 'hle_hid.c').read_text(encoding='utf-8')
        start = src.index('#define RES_CTRL_CANCELED')
        stop = src.index('// Six-axis sensor', start)
        first = src[start:stop]
        start = src.index('HLE_FORCE("_ZN2nn3hid21ShowControllerSupportERK')
        second = src[start:src.index('\n', start)]
        h = (HOST / 'hle.h').read_text(encoding='utf-8')
        applet = h[h.index('void dm_applet_enter('):h.index('// Audio pause')]
        code = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t Result;
#define RES_OK 0
#define DM_PLAYERS 8
#define LOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define HLE_FORCE(symbol, description, result, name, ...) result name(__VA_ARGS__)
static unsigned mask, opens, depth;
static int reported(int p) { return !!(mask & (1u<<p)); }
static uint32_t connected_id(void) { for (int p=0;p<8;++p) if (reported(p)) return p; return 0; }
void dm_applet_enter(const char *name) { assert(name && !depth); depth++; opens++; }
void dm_applet_leave(void) { assert(depth==1); depth--; }
''' + applet + first + second + r'''
int main(void) {
    for (mask=0;mask<256;++mask) for (int min=0;min<=8;++min) for (int max=0;max<=8;++max) {
        int8_t arg[8]={(int8_t)min,(int8_t)max}; int count=0, info[2]={-1,-1};
        for (int p=0;p<8;++p) count+=reported(p);
        Result expected=count<min ? (202u | 3101u<<9) : 0;
        unsigned before=opens;
        assert(h_hid_ShowCtrl(info,arg)==expected && !depth);
        assert(info[0]==(max>0 && count>max ? max : count) && info[1]==(int)connected_id());
        assert(h_hid_ShowCtrl0(arg)==expected && !depth && opens==before+2);
    }
    assert(h_hid_ShowCtrl(NULL,NULL)==0 && !depth);
    return 0;
}
'''
        self.run_c(code)

    def test_sdk_return_values_and_single_notification_acquisition(self):
        def hook(file, name):
            src = (HOST / file).read_text(encoding='utf-8')
            pos = src.index(', ' + name + ',')
            begin = src.rfind('HLE_FORCE(', 0, pos)
            end = src.index('}', src.index('{', pos)) + 1
            return src[begin:end] + '\n'

        time = (HOST / 'hle_time.c').read_text(encoding='utf-8')
        span = time[time.index('static int64_t snap64('):time.index('// How far the user moved')]
        audio = (HOST / 'hle_audren.c').read_text(encoding='utf-8')
        notify = audio[audio.index('static atomic_int g_ad_notify;'):audio.index('// SDK 7 names of the two above')]
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
typedef uint32_t Result;
#define RES_OK 0
#define HLE_FORCE(symbol, description, result, name, ...) result name(__VA_ARGS__)
#define LOG(...) ((void)0)
static atomic_int binds;
static void *bound;
static void hle_bind_system_event(void *event, int signaled) {
    assert(event && !signaled); bound = event; atomic_fetch_add(&binds, 1);
}
''' + span + notify
        code += hook('hle_net.c', 'h_nifm_HandleErr')
        code += hook('hle_misc.c', 'h_pctl_Stereo')
        code += hook('hle_sdk16.c', 'h_pl_BeginVr')
        code += r'''
static atomic_int acquired;
static void *acquire(void *event) {
    Result r = h_ad_Notify(event);
    assert(r == 0 || r == 0x499);
    if (!r) atomic_fetch_add(&acquired, 1);
    return NULL;
}
static void put64(uint8_t *a, int offset, int64_t v) { memcpy(a + offset, &v, 8); }
int main(void) {
    uint8_t a[0xd0] = {0}, b[0xd0] = {0}; int64_t out = 99;
    assert(h_time_SpanBetween(&out, a, b) == 1 && out == 0);
    put64(a, 0x90, 1000); put64(b, 0x90, 1090);
    assert(h_time_SpanBetween(&out, a, b) == 1 && out == 90000000000LL);
    assert(h_time_SpanBetween(&out, b, a) == 1 && out == -90000000000LL);
    b[0x98] = 1; out = 1234;
    assert(h_time_SpanBetween(&out, a, b) == 0 && out == 1234);
    put64(a, 0x48, 300); put64(b, 0x48, 400);
    assert(h_time_SpanBetween(&out, a, b) == 1 && out == 100000000000LL);
    for (int i = 0; i < 10; ++i) {
        assert(h_nifm_HandleErr() == 0);
        assert(h_pctl_Stereo((unsigned)i) == 0);
        assert(h_pl_BeginVr() == 0xd08e);
    }
    int event = 0, other = 0;
    assert(h_ad_Notify(&event) == 0 && binds == 1 && bound == &event);
    assert(h_ad_Notify(&other) == 0x499 && binds == 1 && bound == &event);
    atomic_store(&g_ad_notify, 0); atomic_store(&binds, 0);
    pthread_t threads[32]; int events[32] = {0};
    for (int i = 0; i < 32; ++i) assert(!pthread_create(&threads[i], NULL, acquire, &events[i]));
    for (int i = 0; i < 32; ++i) assert(!pthread_join(threads[i], NULL));
    assert(acquired == 1 && binds == 1 && bound);
    return 0;
}
'''
        self.run_c(code)

    def test_internal_gpu_boundary_retains_conditional_render_state(self):
        src = (ROOT / 'nxnative/dm/android/gfx_vk_exec.c').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define VKLOG(...) ((void)printf(__VA_ARGS__))
static int g_prof_on;
static volatile int g_vk_samp_log;
static uint64_t g_prof_ns[8], g_prof_t0, g_prof_pkg0, g_prof_npkg0, g_prof_pipes0, g_pkg_ns, g_pkg_n, g_tex_frame;
static struct { uint64_t pipes_built; } g_vkst;
static uint64_t tick(void) { return 1; }
static int gettid(void) { return 1; }
static void compute_warm_start(void) {}
#define VK_NULL_HANDLE NULL
static struct { int pass_open; void *bound_pipe; uint64_t logical_state; } S;
static int g_cb_draws, g_ord_parts, g_psplit_now, g_rc_npend, g_rc_skip, g_pipe_predict_scanned, g_pipe_predict_budget;
static uint64_t g_set_epoch;
static unsigned initialized, prewarmed, reads, continuation_reads, logical_reads;
static void state_reset(void) { initialized++; }
static void prewarm_start(void) { prewarmed++; }
static void x_read(int continuation) { reads++; if(continuation)continuation_reads++;else logical_reads++; }
''' + function(src, 'static void exec_boundary(') + function(src, 'void vk_exec_continue(void)') + function(src, 'void vk_exec_reset(void)') + r'''
int main(void) {
    S.logical_state = UINT64_C(0xabcd1234);
    vk_exec_reset(); assert(initialized == 1 && prewarmed == 1 && !g_rc_npend && !g_rc_skip);
    for (int skip=0; skip<2; ++skip) for (int pending=0; pending<257; ++pending) {
        S.pass_open=2; S.bound_pipe=(void *)(uintptr_t)1; g_cb_draws=128; g_ord_parts=5; g_psplit_now=1;
        g_rc_npend=pending; g_rc_skip=skip; uint64_t epoch=g_set_epoch;
        vk_exec_continue();
        assert(!S.pass_open && !S.bound_pipe && !g_cb_draws && !g_ord_parts && !g_psplit_now);
        assert(g_set_epoch == epoch+1 && g_rc_npend == pending && g_rc_skip == skip);
        assert(S.logical_state == UINT64_C(0xabcd1234));
    }
    vk_exec_reset(); assert(!g_rc_npend && !g_rc_skip && initialized == 1 && prewarmed == 1);
    assert(reads == 516 && continuation_reads == 514 && logical_reads == 2); return 0;
}
'''
        self.run_c(code)

    def test_language_aliases_and_exact_system_table(self):
        src = (HOST / 'hle_sdk16.c').read_text(encoding='utf-8')
        start = src.index('static const char *const k_lang_code[]')
        end = src.index('\n}', src.index('h_set_AvailLangN')) + 3
        code = r'''
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
// LanguageCode is a fixed eight-byte value, not necessarily a C string.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
#define HLE_FORCE(symbol, description, result, name, ...) result name(__VA_ARGS__)
''' + src[start:end] + r'''
int main(void) {
    const char *codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    for (int32_t lang = 0; lang < 18; ++lang) for (int i = 0; i < 18; ++i) {
        char code[8] = {0}; memcpy(code, codes[i], strlen(codes[i]));
        unsigned equal = lang == i || (lang == 6 && i == 15) || (lang == 15 && i == 6)
                                  || (lang == 11 && i == 16) || (lang == 16 && i == 11);
        assert(h_set_LangEq(&lang, code) == equal);
        assert(h_set_LangEq2(code, &lang) == equal);
    }
    const int32_t invalid[] = {-1, INT_MIN, 18, INT_MAX};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        assert(!h_set_LangEq(&invalid[i], "zh-Hans"));
        assert(!h_set_LangEq2("zh-Hans", &invalid[i]));
        assert(h_set_LangMake(invalid[i]) == 0);
    }
    const char malformed[][8] = {"", "zh", "zh-hans", {'z','h','-','H','a','n','s','X'},
        {'z','h','-','H','a','n','t','X'}, {'z','h','-','C','N','x','y','z'}, {'z','h','-','T','W','x','y','z'}};
    const int32_t chinese[] = {6, 11, 15, 16};
    for (unsigned i = 0; i < sizeof malformed / sizeof *malformed; ++i)
        for (unsigned j = 0; j < sizeof chinese / sizeof *chinese; ++j) {
            assert(!h_set_LangEq(&chinese[j], malformed[i]));
            assert(!h_set_LangEq2(malformed[i], &chinese[j]));
        }
    char available[19 * 8]; memset(available, 0xa5, sizeof available);
    assert(h_set_AvailLangN() == 18 && h_set_AvailLang(available, 19) == 18);
    for (int i = 0; i < 18; ++i) {
        char expected[8] = {0}; memcpy(expected, codes[i], strlen(codes[i]));
        uint64_t code = h_set_LangMake(i);
        assert(!memcmp(&code, expected, 8));
        assert(!memcmp(available + i * 8, expected, 8));
    }
    for (int i = 18 * 8; i < 19 * 8; ++i) assert((unsigned char)available[i] == 0xa5);
    return 0;
}
'''
        self.run_c(code)

    def test_nvn_cpu_mapping_respects_access_without_changing_gpu_addresses(self):
        folder = ROOT / 'nxnative/dm/gfx/nvn'
        src = (folder / 'hle_nvn.c').read_text(encoding='utf-8')
        obj = (folder / 'nvn_obj.h').read_text(encoding='utf-8')
        header = (folder / 'include/nvn.h').read_text(encoding='utf-8')
        flag = next(line.strip() for line in header.splitlines()
                    if line.strip().startswith('NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS ='))
        types = '\n'.join(line for line in (obj + '\n' + src).splitlines()
                          if line.startswith('typedef struct {') and
                          ('} pool_t;' in line or '} buf_t;' in line))
        hooks = '\n'.join(line for line in src.splitlines()
                          if any(line.startswith('NVN(' + name + ',') for name in
                                 ('nvnBufferMap', 'nvnMemoryPoolMap', 'nvnBufferGetAddress')))
        self.run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define FITS(...)
#define NVN(name, result, ...) result name(__VA_ARGS__)
typedef void NVNbuffer;
typedef void NVNmemoryPool;
typedef uint64_t NVNbufferAddress;
''' + 'enum { ' + flag + ' };\n' + types + r'''
#define BF(b) ((const buf_t *)(b))
''' + hooks + r'''
int main(void) {
    uint8_t storage[128];
    pool_t pool = {.mem = storage, .size = sizeof storage};
    buf_t buf = {.pool = &pool, .off = 24, .size = 48};
    for (unsigned flags = 0; flags < 1024; flags++) {
        pool.flags = flags;
        int inaccessible = flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS;
        assert(nvnMemoryPoolMap(&pool) == (inaccessible ? NULL : storage));
        assert(nvnBufferMap(&buf) == (inaccessible ? NULL : storage + 24));
        assert(nvnBufferGetAddress(&buf) == (uintptr_t)(storage + 24));
        assert(buf.off == 24 && buf.size == 48 && pool.mem == storage);
    }
    return 0;
}
''')

    def test_movie_output_caps_frame_step_without_changing_audio_or_game_mode(self):
        movie = (HOST / 'hle_movie.c').read_text(encoding='utf-8')
        nvn = (ROOT / 'nxnative/dm/gfx/nvn/hle_nvn.c').read_text(encoding='utf-8')
        names = ('MD_NIN ', 'MD_NOUT ', 'MD_TYPE_AVC ', 'MD_TYPE_VP9 ',
                 'MD_TYPE_AAC ', 'MD_TYPE_VORBIS ', 'MD_AUDIO(', 'MD_NO_OUTPUT ')
        definitions = '\n'.join(line for line in movie.splitlines()
                                if any(line.startswith('#define ' + name) for name in names))
        signal = movie[movie.index('#define MD_PLAYING_NS'):movie.index('static void *msym(')]
        acquire = function(movie, 'HLE_FORCE("_ZN5movie7Decoder24AcquireOutputBufferIndex')
        stop = function(movie, 'HLE_FORCE("_ZN5movie7Decoder4StopEv"')
        destroy = function(movie, 'static void md_free(')
        cap = function(nvn, 'static int fps_step_cap(')
        decoder_of = next(line for line in movie.splitlines() if line.startswith('static mdec_t *md_of('))
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#define HLE_FORCE(symbol, description, result, name, ...) result name(__VA_ARGS__)
#define LOG(...) do {} while (0)
#define MD_TRACE(...) do {} while (0)
#define MDJ_QUIT 7
static uint64_t test_now = 1000000000ull;
static uint64_t now_ns(void) { return test_now; }
''' + definitions + r'''
typedef struct {
    int type, started, noutq, outq[MD_NOUT];
    struct { void *frm; int64_t pts; uint32_t flags; } out[MD_NOUT];
    void *in[MD_NIN], *vhdr[2];
    pthread_mutex_t mu, dmu, wmu;
    pthread_cond_t wcv;
    pthread_t wk;
} mdec_t;
static pthread_mutex_t g_dfmu = PTHREAD_MUTEX_INITIALIZER;
static unsigned drops, quits;
static void md_defer_drop(mdec_t *d) { assert(d); drops++; }
static void md_job(mdec_t *d, int job) { assert(d && job == MDJ_QUIT); quits++; }
static void md_reset(mdec_t *d, int reset) { assert(reset); d->noutq = 0; }
static void md_tail(mdec_t *d) { (void)d; }
''' + decoder_of + '\n' + signal + acquire + stop + destroy + r'''
static _Alignas(8) uint8_t state[64];
static uint8_t *dm_fps_state = state;
''' + cap + r'''
static void target(float fps) { memcpy(state, &fps, sizeof fps); }
static void *worker(void *p) { return p; }
static mdec_t *decoder(int type) {
    mdec_t *d = calloc(1, sizeof *d); assert(d);
    d->type = type; d->started = 1;
    assert(!pthread_mutex_init(&d->mu, NULL));
    assert(!pthread_mutex_init(&d->dmu, NULL));
    assert(!pthread_mutex_init(&d->wmu, NULL));
    assert(!pthread_cond_init(&d->wcv, NULL));
    assert(!pthread_create(&d->wk, NULL, worker, NULL)); return d;
}
static void picture(void **self, int slot) {
    mdec_t *d = self[1]; d->outq[0] = slot; d->noutq = 1;
    d->out[slot].pts = 1234; d->out[slot].flags = 2;
    int idx, left; int64_t pts; uint32_t flags;
    assert(!h_movie_AcqOut(self, &idx, &pts, &flags, &left));
    assert(idx == slot && pts == 1234 && flags == 2 && !left);
}
int main(void) {
    target(60); assert(fps_step_cap() == 60 && !dm_movie_playing());
    void *video[2] = {NULL, decoder(MD_TYPE_VP9)};
    void *audio[2] = {NULL, decoder(MD_TYPE_VORBIS)};
    picture(audio, 0); assert(!dm_movie_playing() && fps_step_cap() == 60);
    picture(video, 3); assert(dm_movie_playing() && fps_step_cap() == 30);
    test_now += MD_PLAYING_NS - 1; assert(dm_movie_playing());
    int idx, left; int64_t pts; uint32_t flags;
    assert(h_movie_AcqOut(video, &idx, &pts, &flags, &left) == MD_NO_OUTPUT);
    assert(h_movie_AcqOut(video, NULL, &pts, &flags, &left) == 0x100b);
    test_now++; assert(!dm_movie_playing() && fps_step_cap() == 60);
    picture(video, 2); assert(dm_movie_playing());
    assert(!h_movie_Stop(audio) && dm_movie_playing());
    target(0); assert(fps_step_cap() == 0); target(30); assert(fps_step_cap() == 30);
    target(60); assert(!h_movie_Stop(video) && !dm_movie_playing() && fps_step_cap() == 60);
    ((mdec_t *)video[1])->started = 0;
    ((mdec_t *)video[1])->outq[0] = 0; ((mdec_t *)video[1])->noutq = 1;
    assert(h_movie_AcqOut(video, &idx, &pts, &flags, &left) == MD_NO_OUTPUT);
    assert(!dm_movie_playing());
    ((mdec_t *)video[1])->started = 1; picture(video, 1);
    md_free(audio); assert(!audio[1] && dm_movie_playing());
    md_free(video); assert(!video[1] && !dm_movie_playing() && fps_step_cap() == 60);
    md_free(video); assert(quits == 2 && drops == 4);
    dm_fps_state = NULL; assert(fps_step_cap() == 0); return 0;
}
'''
        self.run_c(code)

    def test_core_handoff_preserves_pinned_order_without_holding_migratable_workers(self):
        src = (HOST / 'hle_thread.c').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>
typedef int qos_class_t;
enum { QOS_CLASS_UNSPECIFIED, O_THREAD };
typedef struct thr {
    int started, exited, host_owned, core, prio, running, wkind, sleeping, woke, last_svc, yield_pending;
    uint32_t handle, qos; uint64_t mask, id, last_lr; int64_t wres;
    pthread_t pt; uintptr_t mport; int *host_work;
    pthread_cond_t cv, pcv; struct thr *all;
} thr_t;
static pthread_key_t g_keythr;
static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
static thr_t *g_all;
static uint64_t g_next_id;
static int dm_host_work;
static void qos_map_init(void) {}
static void pause_install(void) {}
static void hitch_init(void) {}
static uintptr_t pthread_mach_thread_np(pthread_t thread) { (void)thread; return 1; }
static int pthread_get_qos_class_np(pthread_t thread, qos_class_t *qos, int *rel) {
    (void)thread; *qos = 0; *rel = 0; return 0;
}
static uint32_t h_new(int kind, void *object) { (void)kind; (void)object; return 123; }
static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static int thr_running(const thr_t *t) { return t->running; }
static void wait_unlink(thr_t *t) { (void)t; }
typedef struct { const char *name; uint8_t *base; } module_t;
static module_t *module_of(const void *p) { (void)p; return NULL; }
#define LOG(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
''' + src[src.index('typedef struct { thr_t *t; uint32_t handle;'):src.index('static int busy_peer(')] + function(src, 'static int busy_peer(') + function(src, 'int dm_yield_to_core_peers(') + function(src, 'static void wake(') + function(src, 'uint32_t svc_thread_main_init(') + r'''
static void *finish_peer(void *arg) {
    struct timespec delay = {0, 5000000}; nanosleep(&delay, NULL);
    pthread_mutex_lock(&G); ((thr_t *)arg)->running = 0; pthread_mutex_unlock(&G);
    return NULL;
}
int main(void) {
    assert(svc_thread_main_init() == 123);
    thr_t *self = pthread_getspecific(g_keythr);
    assert(self && self->core == 0 && self->mask == 1);
    thr_t peer = {.started = 1, .prio = 30, .running = 1};
    pthread_cond_init(&peer.cv, NULL); self->all = &peer;
    for (int core = 0; core < 4; ++core) {
        self->core = core; peer.core = core; peer.mask = 1ull << core;
        for (uint64_t mask = 1; mask < 16; ++mask) {
            if (!(mask & peer.mask)) continue;
            self->mask = mask; self->yield_pending = 0;
            wake(&peer, 7);
            assert(peer.woke && peer.wres == 7 && peer.wkind == 0);
            assert(self->yield_pending == (mask == peer.mask));
            int result = dm_yield_to_core_peers(200);
            assert(result == (mask == peer.mask ? -1 : 0));
        }
    }
    self->core = peer.core = 0; self->mask = peer.mask = 1;
    peer.host_owned = 1; self->yield_pending = 0; wake(&peer, 0);
    assert(!self->yield_pending && dm_yield_to_core_peers(200) == 0);
    peer.host_owned = 0; peer.prio = 50; self->yield_pending = 0; wake(&peer, 0);
    assert(!self->yield_pending && dm_yield_to_core_peers(200) == 0);
    peer.prio = 30; peer.mask = 5; self->yield_pending = 0; wake(&peer, 0);
    assert(!self->yield_pending && dm_yield_to_core_peers(200) == 0);
    peer.mask = 1;
    pthread_t worker; assert(!pthread_create(&worker, NULL, finish_peer, &peer));
    assert(dm_yield_to_core_peers(500000) == 1);
    pthread_join(worker, NULL);
    assert(dm_yield_to_core_peers(200) == 0);
    pthread_cond_destroy(&peer.cv); pthread_cond_destroy(&self->cv); pthread_cond_destroy(&self->pcv);
    pthread_key_delete(g_keythr); free(self);
    return 0;
}
'''
        self.run_c(code)


if __name__ == '__main__':
    unittest.main()
