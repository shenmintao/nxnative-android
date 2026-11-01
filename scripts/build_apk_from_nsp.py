"""Counterpart of nxnative's build_ipa_from_nsp.py: NSP/XCI -> Android APK (optional external data)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from nxnative_android.__main__ import main

if __name__ == '__main__':
    sys.exit(main(['convert', *sys.argv[1:]]))
