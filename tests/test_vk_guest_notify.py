"""Guest completion must follow CPU effects and precede only internal retirement."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class GuestNotification(unittest.TestCase):
    def setUp(self):
        self.cc = shutil.which('cc') or shutil.which('clang')
        if not self.cc:
            self.skipTest('a host C compiler is required')
        self.core = (ROOT / 'nxnative/dm/android/gfx_vk_core.c').read_text(encoding='utf8')

    def build(self, directory, label, code, flags=(), extra_sources=()):
        source = directory / (label + '.c')
        executable = directory / (label + '.exe')
        source.write_text(code, encoding='utf8')
        result = subprocess.run([self.cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                                 '-pthread', *flags, str(source), *map(str, extra_sources), '-o', str(executable)],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return executable

    def test_guest_effects_and_slot_retirement_have_separate_boundaries(self):
        core = self.core
        completion = function(core, 'static void *completion_main(void *a) {')
        visible = completion.index('completion_notify_guest(c->fence);')
        retired = completion.index('completion_retire(c);')
        self.assertLess(completion.index('wait_fence(c->fence);'), visible)
        self.assertLess(completion.index('pools_copy_back('), visible)
        self.assertLess(completion.rindex('vkGetQueryPoolResults('), visible)
        self.assertLess(completion.rindex('memcpy('), visible)
        self.assertLess(visible, completion.index('g_ch = c->next'))
        self.assertLess(visible, retired)
        self.assertLess(retired, completion.index('free(c->pmeta)'))
        start = core.index('static pthread_mutex_t g_done_mu')
        state = core[start:core.index('static void slots_init(void) {', start)]
        retire = function(core, 'static void completion_retire(cpend *c) {')
        wait = function(core, 'int mtl_wait(uint64_t fence, uint64_t timeout_ns) {')
        guest = function(core, 'int vk_guest_wait(uint64_t fence, uint64_t timeout_ns) {')
        done = function(core, 'uint64_t vk_fence_done(void) {')
        prefix, suffix = (ROOT / 'tests/native/vk_guest_notify.c').read_text(encoding='utf8').split('/* PRODUCTION HELPERS */')
        line = 'pthread_mutex_lock(&g_done_mu); if (c->fence > g_done) __atomic_store_n(&g_done, c->fence, __ATOMIC_RELEASE); pthread_cond_broadcast(&g_done_cv); pthread_mutex_unlock(&g_done_mu);'
        self.assertEqual(retire.count(line), 1)
        early = retire.replace(line, '').replace('static void completion_retire(cpend *c) {',
                                                'static void completion_retire(cpend *c) {\n    ' + line)
        variants = [
            ('actual', state, retire, guest, False),
            ('late-guest', state, retire, guest.replace('g_guest_done < fence', 'g_done < fence').replace('g_guest_done >= fence', 'g_done >= fence'), True),
            ('early-slot', state, early, guest, True),
            ('ignores-opt-in', state.replace('return v == 1;', 'return 1;'), retire, guest, True),
        ]
        with tempfile.TemporaryDirectory(prefix='vk-guest-notify-') as temp:
            for label, selected_state, selected_retire, selected_guest, negative in variants:
                with self.subTest(variant=label):
                    trace = '#include "' + str(ROOT / 'nxnative/dm/android/sync_trace.h').replace('\\', '/') + '"\n'
                    code = trace + prefix + '\n'.join((selected_state, selected_retire, wait, selected_guest, done)) + suffix
                    executable = self.build(Path(temp), label, code)
                    failures = 0
                    for flag, enabled in ((None, 0), ('0', 0), ('1', 1)):
                        env = dict(os.environ)
                        env.pop('DM_VK_GUEST_NOTIFY', None)
                        if flag is not None:
                            env['DM_VK_GUEST_NOTIFY'] = flag
                        result = subprocess.run([str(executable), str(enabled)], env=env,
                                                capture_output=True, text=True, timeout=25)
                        failures += result.returncode != 0
                        if not negative:
                            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    if negative:
                        self.assertGreater(failures, 0, 'the boundary oracle accepted ' + label)

    def test_non_android_and_optional_backend_fallback(self):
        source = (ROOT / 'nxnative/dm/gfx/nvn/hle_nvn.c').read_text(encoding='utf8')
        wrapper = function(source, 'static int nvn_backend_fence_wait(uint64_t f, uint64_t timeout_ns) {')
        with tempfile.TemporaryDirectory(prefix='vk-guest-fallback-') as temp:
            for android, present in ((False, False), (True, False), (True, True)):
                with self.subTest(android=android, backend=present):
                    code = '#include <stdint.h>\nint mtl_wait(uint64_t f,uint64_t t) {return (int)(f+t+10);}\n'
                    extra_sources = ()
                    if present:
                        backend = Path(temp) / 'backend.c'
                        backend.write_text('#include <stdint.h>\nint vk_guest_wait(uint64_t f,uint64_t t) {return (int)(f+t+20);}\n', encoding='utf8')
                        extra_sources = (backend,)
                    expected = 27 if android and present else 17
                    code += wrapper + '\nint main(void) {return nvn_backend_fence_wait(3,4)==' + str(expected) + '?0:86;}\n'
                    executable = self.build(Path(temp), 'fallback', code, ('-DDM_ANDROID=1',) if android else (), extra_sources)
                    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
