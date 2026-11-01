"""Rebuild the small UI bridge shipped with the APK writer; not needed to port a game."""
import argparse
import hashlib
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BRIDGE = ROOT / 'nxnative/dm/android/apk/bridge'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--platform', default='android-36')
    parser.add_argument('--build-tools', default='36.1.0')
    args = parser.parse_args()
    android = args.sdk / 'platforms' / args.platform / 'android.jar'
    d8 = args.sdk / 'build-tools' / args.build_tools / ('d8.bat' if os.name == 'nt' else 'd8')
    javac = shutil.which('javac')
    if not javac or not android.is_file() or not d8.is_file():
        parser.error('JDK and the requested Android SDK platform/build-tools are required')
    with tempfile.TemporaryDirectory(prefix='nxnative_bridge_') as work:
        classes = Path(work) / 'classes'
        dex = Path(work) / 'dex'
        classes.mkdir(); dex.mkdir()
        subprocess.run([javac, '--release', '8', '-g:none', '-encoding', 'UTF-8', '-classpath', str(android), '-d', str(classes), *map(str, sorted(BRIDGE.glob('*.java')))], check=True)
        subprocess.run([str(d8), '--release', '--min-api', '29', '--lib', str(android), '--output', str(dex), *map(str, sorted(classes.rglob('*.class')))], check=True)
        shutil.copyfile(dex / 'classes.dex', BRIDGE / 'classes.dex')
    print('classes.dex SHA-256:', hashlib.sha256((BRIDGE / 'classes.dex').read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
