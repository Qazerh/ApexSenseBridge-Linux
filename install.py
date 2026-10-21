#!/usr/bin/env python3
"""Build the Linux bridge and install the unified per-user application."""
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parent
PREFIX=Path.home()/'.local'
DATA=PREFIX/'share/ApexSenseBridge'
subprocess.run(['cmake','-S',str(ROOT),'-B',str(ROOT/'build'),
    '-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=OFF',f'-DCMAKE_INSTALL_PREFIX={PREFIX}'],check=True)
subprocess.run(['cmake','--build',str(ROOT/'build'),'--parallel','4'],check=True)
subprocess.run(['cmake','--install',str(ROOT/'build')],check=True)
DATA.mkdir(parents=True,exist_ok=True)
for name in ('gui','vendor'):
    shutil.copytree(ROOT/name,DATA/name,dirs_exist_ok=True,
        ignore=shutil.ignore_patterns('__pycache__','.git','backups'))
for obsolete in ('gui/console_ui.py','gui/hardware_panel.py','gui/tests/test_hardware_integration.py','openflydigi/gui/qml/ApexSensePanel.qml'):
    (DATA/obsolete).unlink(missing_ok=True)
shutil.copytree(ROOT/'packaging/linux', DATA/'packaging/linux', dirs_exist_ok=True)
for name in ('VERSION','LICENSE','SOURCE-REVISIONS.txt','setup-devices.sh'):
    shutil.copy2(ROOT/name, DATA/name)
launcher=PREFIX/'bin/apexsense'
launcher.parent.mkdir(parents=True,exist_ok=True)
launcher.write_text('#!/bin/sh\nexec /usr/bin/python3 "'+str(DATA/'gui/main.py')+'" "$@"\n')
launcher.chmod(0o755)
desktop=PREFIX/'share/applications/apexsensebridge.desktop'
desktop.parent.mkdir(parents=True,exist_ok=True)
desktop.write_text('[Desktop Entry]\nType=Application\nName=ApexSenseBridge\n'
    'Comment=Профили Flydigi и игровой мост DualSense\n'
    f'Exec="{launcher}"\nTerminal=false\nIcon=input-gaming\nCategories=Game;\nStartupWMClass=ApexSenseBridge\n')
print('Installed. Launch ApexSenseBridge from the applications menu, or run',launcher)
