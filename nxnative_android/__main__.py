import os
import sys
from pathlib import Path


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    # Keep the tools, settings and signing key on the project's drive by default.
    # Respect an existing NX2A_HOME, including one shared with nx2android.
    root = Path(__file__).resolve().parent.parent
    os.environ.setdefault('NX2A_HOME', str(root / 'tools/player'))
    if not argv or argv == ['gui']:
        from gui.app import main as gui_main
        return gui_main([sys.argv[0]])
    if argv[0] == 'convert':
        argv[0] = 'port'
    from porter.cli import main as cli_main
    return cli_main(argv)


if __name__ == '__main__':
    sys.exit(main())
