import copy
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import contextlib

from alyx_macos.core import Error, Layout, REPO, extract, write_json
from alyx_macos.configuration import apply_profile, change_settings, game_flags
from alyx_macos.configuration import wine_registry_values
from alyx_macos.cli import main
from alyx_macos.runtime import require_no_other_vr

class ToolsTests(unittest.TestCase):
    def test_zip_escape_is_rejected_before_outside_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); archive=root/'bad.zip'
            with zipfile.ZipFile(archive,'w') as z:z.writestr('../outside.txt','bad')
            with self.assertRaises(Error):extract(archive,root/'out')
            self.assertFalse((root/'outside.txt').exists())

    def test_tar_external_link_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); archive=root/'bad.tar'
            with tarfile.open(archive,'w') as t:
                item=tarfile.TarInfo('escape');item.type=tarfile.SYMTYPE;item.linkname='../outside'
                t.addfile(item)
            with self.assertRaises(Error):extract(archive,root/'out')
            self.assertFalse((root/'out/escape').exists())

    def test_safe_zip_retains_contents(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); archive=root/'ok.zip'
            with zipfile.ZipFile(archive,'w') as z:z.writestr('bin/file.txt','hello')
            extract(archive,root/'out')
            self.assertEqual((root/'out/bin/file.txt').read_text(),'hello')

    def test_unmanaged_existing_folder_not_adopted(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'personal.txt').write_text('keep')
            with self.assertRaises(Error):Layout(root).create()
            self.assertEqual((root/'personal.txt').read_text(),'keep')
            self.assertFalse((root/'.alyx-macos.json').exists())

    def test_managed_paths_cannot_escape_to_another_app(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);layout=Layout(root/'own');layout.create()
            other=root/'other.app';other.mkdir();layout.app.symlink_to(other,target_is_directory=True)
            with self.assertRaises(Error):layout.managed()

    def test_active_prefix_refuses_settings_without_writing(self):
        with tempfile.TemporaryDirectory() as tmp:
            layout=Layout(Path(tmp)/'own');layout.create()
            write_json(layout.prefs,layout.preferences());before=layout.prefs.read_bytes()
            with patch('alyx_macos.core.owned_pids',return_value=[123]):
                with self.assertRaises(Error):change_settings(layout,{'msaa':4})
            self.assertEqual(layout.prefs.read_bytes(),before)
            self.assertFalse((layout.root/'backups').exists())

    def test_unsupported_geometry_does_not_commit(self):
        with tempfile.TemporaryDirectory() as tmp:
            layout=Layout(Path(tmp)/'own');layout.create()
            write_json(layout.prefs,layout.preferences());before=layout.prefs.read_bytes()
            with patch('alyx_macos.core.owned_pids',return_value=[]):
                with self.assertRaises(Error):change_settings(layout,{'render_eye':[1636,1636]})
            self.assertEqual(layout.prefs.read_bytes(),before)

    def test_profile_preserves_clients_and_unrelated_fields(self):
        session=json.loads((REPO/'config/alvr-session-template.json').read_text())
        session['client_connections']={'demo.client':{'trusted':False}}
        session['session_settings']['video']['color_correction']['content']['brightness']=.123
        original=copy.deepcopy(session)
        prefs=json.loads((REPO/'config/profiles.json').read_text())['performance90']
        apply_profile(session,prefs)
        self.assertEqual(session['client_connections'],original['client_connections'])
        self.assertEqual(session['session_settings']['video']['color_correction'],
                         original['session_settings']['video']['color_correction'])
        self.assertEqual(session['openvr_config']['refresh_rate'],90)
        self.assertEqual(session['session_settings']['video']['preferred_fps'],90)
        self.assertFalse(session['session_settings']['video']['enforce_server_frame_pacing'])

    def test_launch_flags_keep_game_quality_explicit(self):
        flags=game_flags({'msaa':4},'s0/quick')
        self.assertEqual(flags[flags.index('+vr_msaa')+1],'4')
        self.assertEqual(flags[flags.index('+vr_fidelity_level_auto')+1],'0')
        self.assertIn('-dx11',flags)
        self.assertEqual(flags[-2:],['+load','s0/quick'])

    def test_dry_run_never_launches_or_creates_installation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'not-created'
            with patch('alyx_macos.runtime.launch') as launch,contextlib.redirect_stdout(io.StringIO()) as out:
                code=main(['launch','--root',str(root),'--dry-run'])
            self.assertEqual(code,0);launch.assert_not_called();self.assertFalse(root.exists())
            self.assertFalse(json.loads(out.getvalue())['action_performed'])

    def test_other_vr_prefix_is_refused_not_stopped(self):
        with tempfile.TemporaryDirectory() as tmp:
            layout=Layout(Path(tmp)/'ours')
            result=type('R',(),{'stdout':'234 C:/Steam/vrserver.exe\n'})()
            with patch('alyx_macos.runtime.subprocess.run',return_value=result),\
                 patch('alyx_macos.runtime.process_prefix',return_value=Path(tmp)/'other'),\
                 patch('alyx_macos.runtime.os.kill') as kill:
                with self.assertRaises(Error):require_no_other_vr(layout)
                kill.assert_not_called()

    def test_wine_dpi_retina_and_crt_are_explicit(self):
        values={(key,name):(kind,value) for key,name,kind,value in wine_registry_values()}
        self.assertEqual(values[(r'HKCU\Software\Wine\Mac Driver','RetinaMode')],('REG_SZ','Y'))
        self.assertEqual(values[(r'HKCU\Control Panel\Desktop','LogPixels')],('REG_DWORD','192'))
        self.assertEqual(values[(r'HKCU\Control Panel\Desktop','FontSmoothing')],('REG_SZ','2'))
        self.assertEqual(values[(r'HKCU\Software\Wine\AppDefaults\vrserver.exe\DllOverrides','msvcp140')],('REG_SZ','native,builtin'))

    def test_shortcuts_preserve_typed_existing_records_and_are_idempotent(self):
        from alyx_macos.steam_shortcut import Entry,parse,serialize,add_entry,string
        import struct
        original=[Entry(0,'shortcuts',[Entry(0,'0',[
            string('AppName','Existing app'),Entry(7,'uint64',struct.pack('<Q',2**63+1)),
            Entry(3,'float',struct.pack('<f',.25)),Entry(0,'tags',[string('0','keep')])])])]
        self.assertEqual(serialize(parse(serialize(original))),serialize(original))
        data=parse(serialize(original));self.assertTrue(add_entry(data));self.assertFalse(add_entry(data))
        self.assertEqual(serialize([data[0].payload[0]]),serialize([original[0].payload[0]]))
        self.assertEqual(len(data[0].payload),2)

    def test_bad_shortcuts_are_rejected(self):
        from alyx_macos.steam_shortcut import parse
        for data in [b'\x00shortcuts\0',b'\x09unknown\0',b'\x08trailing']:
            with self.assertRaises(Error):parse(data)

    def test_steam_stability_restarts_clock_after_process_exit(self):
        from alyx_macos.runtime import ensure_steam_stable
        clock=[0.]
        def sleep(seconds):clock[0]+=seconds
        states=iter([[111],[],[222],[222],[222]])
        with tempfile.TemporaryDirectory() as tmp:
            layout=Layout(Path(tmp)/'ours')
            with patch('alyx_macos.runtime.owned_pids',side_effect=lambda *a:next(states)),\
                 patch('alyx_macos.runtime.start_steam',return_value=222) as start,\
                 patch('alyx_macos.runtime.time.monotonic',side_effect=lambda:clock[0]),\
                 patch('alyx_macos.runtime.time.sleep',side_effect=sleep):
                self.assertEqual(ensure_steam_stable(layout,stable_seconds=1,timeout=5),222)
                start.assert_called_once()

if __name__=='__main__':unittest.main()
