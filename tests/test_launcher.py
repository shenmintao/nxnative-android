"""Resolve the actual launcher instead of assuming a NativeActivity class or DEFAULT filter."""
import unittest
from porter.adb import Adb


class Launcher(unittest.TestCase):
    def test_preparing_files_launches_the_resolved_component(self):
        for activity in ('org.nxnative.GameNativeActivity', 'android.app.NativeActivity'):
            with self.subTest(activity=activity):
                class Device(Adb):
                    def __init__(self): self.commands = []; self.exists = False
                    def shell(self, command, **kwargs):
                        self.commands.append(command)
                        if command.startswith('[ -d '): return 'yes' if self.exists else ''
                        if command.startswith('cmd package resolve-activity'):
                            return 'priority=0 isDefault=false\norg.nxnative.fixture/' + activity
                        if command.startswith('am start '): self.exists = True
                        return ''
                device = Device()
                self.assertTrue(device.prepare_app_files('org.nxnative.fixture'))
                self.assertTrue(any('-n org.nxnative.fixture/' + activity in command for command in device.commands))


if __name__ == '__main__': unittest.main()
