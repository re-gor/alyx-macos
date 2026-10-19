import contextlib
import io
from pathlib import Path
import plistlib
import tempfile
import unittest
from unittest.mock import patch

from alyx_macos.cli import main
from alyx_macos.core import Layout


class SteamStartupTests(unittest.TestCase):
    def make_wrapper(self, root, **overrides):
        layout = Layout(root/'own')
        layout.create()
        info = layout.app/'Contents/Info.plist'
        info.parent.mkdir(parents=True)
        value = {'Program Name and Path': '/Program Files (x86)/Steam/Steam.exe',
                 'Program Flags': '-silent', 'WINEMSYNC': 1, 'WINEESYNC': 1,
                 'CLI Custom Commands': 'keep existing audio/graphics hooks'}
        value.update(overrides)
        info.write_bytes(plistlib.dumps(value))
        return layout, info, value

    def test_upgrade_preserves_other_settings_and_backup_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            layout, info, original = self.make_wrapper(Path(tmp))
            before = info.read_bytes()
            with patch('alyx_macos.core.owned_pids', return_value=[]), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main(['fix-steam-startup', '--root', str(layout.root)]), 0)
                after = info.read_bytes()
                self.assertEqual(main(['fix-steam-startup', '--root', str(layout.root)]), 0)
            self.assertEqual(plistlib.loads(after), {**original, 'Program Flags': '-silent -vrdisable'})
            self.assertEqual(info.read_bytes(), after)
            backups = list((layout.root/'backups').glob('steam-startup-*'))
            self.assertEqual(len(backups), 1)
            self.assertEqual((backups[0]/'0-Info.plist').read_bytes(), before)
            self.assertFalse(layout.session.exists())

    def test_live_prefix_and_non_steam_program_are_rejected_without_writes(self):
        for running, program in [(True, '/Program Files (x86)/Steam/Steam.exe'),
                                 (False, '/ALVR/ALVR Dashboard.exe')]:
            with self.subTest(running=running), tempfile.TemporaryDirectory() as tmp:
                layout, info, _ = self.make_wrapper(Path(tmp), **{'Program Name and Path': program})
                before = info.read_bytes()
                with patch('alyx_macos.core.owned_pids', return_value=[123] if running else []), \
                     contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(main(['fix-steam-startup', '--root', str(layout.root)]), 1)
                self.assertEqual(info.read_bytes(), before)
                self.assertFalse((layout.root/'backups').exists())

    def test_dry_run_does_not_create_or_launch_anything(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)/'not-created'
            with patch('alyx_macos.cli.fix_steam_startup') as fix, \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main(['fix-steam-startup', '--root', str(root), '--dry-run']), 0)
            fix.assert_not_called()
            self.assertFalse(root.exists())


if __name__ == '__main__':
    unittest.main()
