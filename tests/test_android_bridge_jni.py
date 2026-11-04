"""The Java bridge's native methods resolve in the host library, and the checked-in DEX carries them."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / 'nxnative/dm/android'


class BridgeJni(unittest.TestCase):
    def test_every_native_method_has_an_export_and_is_in_the_dex(self):
        java = (ANDROID / 'apk/bridge/GameNativeActivity.java').read_text(encoding='utf8')
        native = (ANDROID / 'front_android.c').read_text(encoding='utf8')
        dex = (ANDROID / 'apk/bridge/classes.dex').read_bytes()
        methods = re.findall(r'private native void (\w+)\(', java)
        self.assertIn('setOverlayInsets', methods)
        self.assertIn('setDisplayRotation', methods)
        for name in methods:
            self.assertRegex(native, r'JNIEXPORT void JNICALL Java_org_nxnative_GameNativeActivity_' + name + r'\(JNIEnv \*env, jobject self')
            self.assertIn(name.encode(), dex, name + ' missing from the rebuilt classes.dex')

    def test_rotation_follows_display_changes_without_a_new_window(self):
        java = (ANDROID / 'apk/bridge/GameNativeActivity.java').read_text(encoding='utf8')
        self.assertIn('registerDisplayListener(displayListener', java)
        self.assertIn('unregisterDisplayListener(displayListener)', java)
        self.assertIn('public void onDisplayChanged(int id) { reportRotation(); }', java)
        self.assertLess(java.index('System.loadLibrary(library)'), java.index('registerDisplayListener('))   # natives resolve before the first callback
        native = (ANDROID / 'front_android.c').read_text(encoding='utf8')
        start = native.index('JNIEXPORT void JNICALL Java_org_nxnative_GameNativeActivity_setDisplayRotation(')
        self.assertIn('motion_rotation_set(rotation);', native[start:native.index('\n}', start)])
        setter = native[native.index('static void motion_rotation_set(int rotation) {'):]
        setter = setter[:setter.index('\n}') + 3]
        self.assertIn('if (rotation < 0 || rotation > 3) return;', setter)
        self.assertIn('dm_motion_clear_switch()', setter)   # the fused pose starts again under the new axes


if __name__ == '__main__':
    unittest.main()
