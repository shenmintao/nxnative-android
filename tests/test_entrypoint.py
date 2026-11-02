"""The public launcher must use the bundle's drive and retain explicit player homes."""
import os
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from nxnative_android.__main__ import main


class Entrypoint(unittest.TestCase):
    def test_frozen_home_is_beside_executable_and_convert_reaches_port(self):
        cli = Mock(return_value=0)
        executable = str(Path('bundle/nxnative-android.exe').resolve())
        with patch.dict(os.environ), patch.dict(sys.modules, {'porter.cli': SimpleNamespace(main=cli)}), \
                patch.object(sys, 'frozen', True, create=True), patch.object(sys, 'executable', executable):
            os.environ.pop('NX2A_HOME', None)
            self.assertEqual(main(['convert', 'game.xci', '--data-mode', 'external']), 0)
            self.assertEqual(Path(os.environ['NX2A_HOME']), Path(executable).parent / 'tools/player')
        cli.assert_called_once_with(['port', 'game.xci', '--data-mode', 'external'])

    def test_configured_home_and_cli_exit_status_are_preserved(self):
        cli = Mock(return_value=7)
        with patch.dict(os.environ, {'NX2A_HOME': 'existing-player-home'}), \
                patch.dict(sys.modules, {'porter.cli': SimpleNamespace(main=cli)}):
            self.assertEqual(main(['doctor']), 7)
            self.assertEqual(os.environ['NX2A_HOME'], 'existing-player-home')
        cli.assert_called_once_with(['doctor'])

    def test_explicit_gui_uses_gui_entrypoint(self):
        gui = Mock(return_value=0)
        with patch.dict(os.environ), patch.dict(sys.modules, {'gui.app': SimpleNamespace(main=gui)}):
            self.assertEqual(main(['gui']), 0)
        gui.assert_called_once_with([sys.argv[0]])
