"""Physical phone sizes, spacing/insets and frozen tablet geometry."""
from pathlib import Path
import math
import shutil
import subprocess
import tempfile
import unittest
from porter.upstream import android_profile

ROOT = Path(__file__).resolve().parents[1]


def compile_layout(folder):
    cc = shutil.which('cc') or shutil.which('clang')
    if not cc:
        raise unittest.SkipTest('host compiler required')
    (folder / 'android_profile.h').write_text(android_profile.header(None), encoding='utf8')
    source = (ROOT / 'nxnative/dm/android/gfx_vk_present.c').read_text(encoding='utf8')
    start = source.index('static float ov_control_sprite_scale(')
    helper = source[start:source.index('\n}', start) + 3]
    check = folder / 'layout.c'
    check.write_text((ROOT / 'tests/native/overlay_phone.c').read_text().replace('/* SPRITE_SCALE */', helper), encoding='utf8')
    exe = folder / 'layout.exe'
    r = subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                        '-I' + str(folder), '-I' + str(ROOT / 'nxnative/dm/android'), '-I' + str(ROOT / 'nxnative/dm/host'),
                        str(check), '-lm', '-o', str(exe)], capture_output=True, text=True)
    if r.returncode:
        raise AssertionError(r.stdout + r.stderr)
    return exe


def layout(exe, width, height, dpi, insets, percent=100, legacy=False):
    r = subprocess.run([str(exe), str(width), str(height), str(dpi), *map(str, insets), str(percent), str(int(legacy))],
                       capture_output=True, text=True, check=True)
    rows = []
    for line in r.stdout.splitlines():
        name, kind, *box = line.split()
        rows.append(dict(name=name, kind=int(kind), box=list(map(float, box))))
    return rows


def separation(a, b):
    x, y, hw, hh = a['box']
    xx, yy, ww, hhh = b['box']
    dx, dy = abs(x - xx), abs(y - yy)
    ra = min(hw, hh) * (1 if abs(hw - hh) < .001 or a['kind'] == 4 else .35)
    rb = min(ww, hhh) * (1 if abs(ww - hhh) < .001 or b['kind'] == 4 else .35)
    r = ra + rb
    qx, qy = dx - hw - ww + r, dy - hh - hhh + r
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


class PhoneOverlay(unittest.TestCase):
    def test_physical_size_spacing_insets_and_tablet_unchanged(self):
        with tempfile.TemporaryDirectory(prefix='overlay-phone-') as temp:
            exe = compile_layout(Path(temp))
            for width, height, dpi, insets in [(2800, 1260, 560, (88, 0, 0, 56)), (2800, 1260, 560, (0, 0, 88, 56)),
                                                (2800, 1260, 560, (88, 96, 0, 100)), (2400, 1080, 560, (80, 0, 0, 56))]:
                for percent in (80, 90, 100, 110, 120):
                    rows = layout(exe, width, height, dpi, insets, percent)
                    gap, margin = dpi / 25.4 * 1.5, dpi / 25.4 * 2
                    for i, a in enumerate(rows):
                        x, y, hw, hh = a['box']
                        self.assertGreaterEqual(x - hw, insets[0] + margin - .05, a['name'])
                        self.assertLessEqual(x + hw, width - insets[2] - margin + .05, a['name'])
                        self.assertGreaterEqual(y - hh, insets[1] + margin - .05, a['name'])
                        self.assertLessEqual(y + hh, height - insets[3] - margin + .05, a['name'])
                        for b in rows[i + 1:]:
                            self.assertGreaterEqual(separation(a, b), gap - .05,
                                                    f'{width}x{height} {percent}%: {a["name"]}/{b["name"]}')
                    face = next(r for r in rows if r['name'] == 'a')['box']
                    self.assertAlmostEqual(face[2] * 2, 10 * dpi / 25.4 * percent / 100, places=3)
            args = (3040, 1904, 440, (0, 0, 0, 56))
            new, old = layout(exe, *args), layout(exe, *args, legacy=True)
            self.assertEqual(new, old)
            for i, a in enumerate(new):
                for b in new[i + 1:]:
                    self.assertGreaterEqual(separation(a, b), -.05, f'{a["name"]}/{b["name"]}')
