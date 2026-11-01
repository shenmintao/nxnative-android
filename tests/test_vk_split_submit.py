"""Deferred parts own begin data and preserve upload, submit and error ordering."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


class DeferredSubmission(unittest.TestCase):
    def test_owned_jobs_upload_order_errors_and_pool_access(self):
        cc = shutil.which('cc') or shutil.which('clang')
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not cc or not headers:
            self.skipTest('C compiler and Vulkan headers required')
        folder = ROOT / 'nxnative/dm/android'
        encoder = (folder / 'gfx_vk_encode.c').read_text(encoding='utf8')
        core = (folder / 'gfx_vk_core.c').read_text(encoding='utf8')
        header = (folder / 'gfx_vk.h').read_text(encoding='utf8')
        support = encoder[encoder.index('enum { VKE_BYTES'):encoder.index('// Pointer-bearing calls below')]
        internal = encoder[encoder.index('// Internal checked operations'):encoder.index('static VkResult VKAPI_CALL vke_vkBeginCommandBuffer(')]
        wrappers = ''.join(function(encoder, 'static VkResult VKAPI_CALL ' + name + '(') for name in
                           ('vke_vkBeginCommandBuffer', 'vke_vkEndCommandBuffer', 'vke_vkAllocateCommandBuffers', 'vke_vkQueueSubmit'))
        start = header.index('typedef struct {\n    int end_error')
        state = header[start:header.index('int vk_encode_deferred_parts(void);', start)]
        split = ''.join(function(core, name) for name in ('static void split_advance_upload(', 'static void split_collect(',
                                                        'static int split_defer(', 'void vk_cmd_split(', 'int vk_sub_sent_early('))
        prefix, rest = (ROOT / 'tests/native/vk_split_submit.c').read_text(encoding='utf8').split('/* ENCODER IMPLEMENTATION */')
        fixture, suffix = rest.split('/* SPLIT IMPLEMENTATION */')
        variants = [
            ('actual', internal, split),
            ('missing-copy-drain', internal, split.replace('    ucopy_drain();\n', '')),
            ('missing-error-collection', internal, split.replace('s->cb_err |= s->deferred_part.end_error;', '')),
            ('missing-queue-mutex', internal.replace('    pthread_mutex_lock(&g_vk_queue_mu);', '').replace('    pthread_mutex_unlock(&g_vk_queue_mu);', ''), split),
            ('reversed-upload-order', internal.replace('    if (a->up) cbs[count++] = a->up;\n    cbs[count++] = a->main;',
                                                       '    cbs[count++] = a->main;\n    if (a->up) cbs[count++] = a->up;'), split),
            ('borrowed-begin-data', internal.replace('real_vkBeginCommandBuffer(a->cb, &a->info)',
                                                     'real_vkBeginCommandBuffer(a->cb, g_test_borrowed_info)'), split),
        ]
        env = dict(os.environ, DM_VK_ENCODE_THREAD='1')
        env.pop('DM_VK_SUBMIT_SPLIT', None)
        with tempfile.TemporaryDirectory(prefix='vk-split-') as temp:
            directory = Path(temp)
            for label, driver, backend in variants:
                with self.subTest(variant=label):
                    if label != 'actual':
                        self.assertNotEqual((driver, backend), (internal, split), 'negative control did not change the source')
                    source = directory / (label + '.c')
                    executable = directory / (label + '.exe')
                    source.write_text(prefix + state + support + driver + wrappers + fixture + backend + suffix, encoding='utf8')
                    built = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                                            '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-value',
                                            '-Wno-missing-field-initializers', '-idirafter', str(headers),
                                            str(source), '-o', str(executable)], capture_output=True, text=True, timeout=40)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    failures = 0
                    for value, enabled in ((None, 0), ('0', 0), ('1', 1)):
                        selected = dict(env)
                        selected.pop('DM_VK_DEFER_PARTS', None)
                        if value is not None:
                            selected['DM_VK_DEFER_PARTS'] = value
                        result = subprocess.run([str(executable), str(enabled), 'none'], env=selected,
                                                capture_output=True, text=True, timeout=25)
                        failures += result.returncode != 0
                        if label == 'actual':
                            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    if label != 'actual':
                        self.assertGreater(failures, 0, 'the driver oracle accepted ' + label)
                    else:
                        for mode in ('begin', 'submit'):
                            for value in ('0', '1'):
                                result = subprocess.run([str(executable), value, mode], env=dict(env, DM_VK_DEFER_PARTS=value),
                                                        capture_output=True, text=True, timeout=15)
                                self.assertEqual(result.returncode, 86, result.stdout + result.stderr)
                                self.assertIn('-1' if mode == 'begin' else '-4', result.stderr)


if __name__ == '__main__':
    unittest.main()
