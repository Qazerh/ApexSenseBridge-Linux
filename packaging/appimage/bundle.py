#!/usr/bin/env python3
"""Build a self-contained AppDir from Debian's matching Python/Qt/KF packages."""
import argparse,os,re,shutil,subprocess,sys,sysconfig
from pathlib import Path
from PySide6.QtCore import QLibraryInfo
from PIL import Image
parser=argparse.ArgumentParser();parser.add_argument('--appdir',type=Path,required=True);parser.add_argument('--engine',type=Path,required=True)
args=parser.parse_args();dest=args.appdir;root=Path(__file__).resolve().parents[2]

def copytree(src,target):
    shutil.copytree(src,target,dirs_exist_ok=True,ignore=shutil.ignore_patterns('__pycache__','.git','*.pyc'))

for path in ['usr/bin','usr/lib','usr/share/ApexSense','usr/share/fonts','etc/fonts']:(dest/path).mkdir(parents=True,exist_ok=True)
shutil.copy2(sys.executable,dest/'usr/bin/python3')
shutil.copy2(args.engine,dest/'usr/bin/ApexSenseBridge')
stdlib=Path(sysconfig.get_path('stdlib'))
copytree(stdlib,dest/'usr/lib'/stdlib.name)
site=dest/'usr/lib/python3/dist-packages';site.mkdir(parents=True,exist_ok=True)
for name in ['PySide6','shiboken6','PIL']:
    module=__import__(name)
    copytree(Path(module.__file__).parent,site/name)
copytree(QLibraryInfo.path(QLibraryInfo.QmlImportsPath),dest/'usr/qml')
copytree(QLibraryInfo.path(QLibraryInfo.PluginsPath),dest/'usr/plugins')
for name in ['gui','vendor']:
    copytree(root/name,dest/'usr/share/ApexSense'/name)
for name in ['VERSION','LICENSE','SOURCE-REVISIONS.txt','setup-devices.sh']:
    shutil.copy2(root/name,dest/'usr/share/ApexSense'/name)
copytree(root/'packaging/linux',dest/'usr/share/ApexSense/packaging/linux')
copytree('/usr/share/fonts/truetype/dejavu',dest/'usr/share/fonts/dejavu')
(dest/'etc/fonts/fonts.conf').write_text('<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd"><fontconfig><dir prefix="relative">../../usr/share/fonts</dir><cachedir prefix="xdg">fontconfig</cachedir><config><rescan><int>0</int></rescan></config></fontconfig>\n')
licenses=dest/'usr/share/doc';licenses.mkdir(parents=True,exist_ok=True)
for package in Path('/usr/share/doc').iterdir():
    copyright_file=package/'copyright'
    if copyright_file.is_file():
        target=licenses/package.name;target.mkdir(exist_ok=True)
        shutil.copy2(copyright_file,target/'copyright')
copytree('/usr/share/common-licenses',dest/'usr/share/common-licenses')

# ldd follows DT_NEEDED transitively, including the dependencies of QML/plugins
# and Python extension modules. Host glibc and graphics drivers stay on the host.
skip=re.compile(r'^(ld-linux|lib(c|m|pthread|dl|rt|util|resolv)\.so\.|libnss_)')
def elf(path):
    if not path.is_file():return False
    with path.open('rb') as stream:return stream.read(4)==b'\x7fELF'
seeds=[p for p in dest.rglob('*') if elf(p)]
seen=set()
for seed in seeds:
    result=subprocess.run(['ldd',str(seed)],capture_output=True,text=True)
    if 'not found' in result.stdout:raise SystemExit(str(seed)+'\n'+result.stdout)
    for line in result.stdout.splitlines():
        match=re.search(r'=> (/\S+)',line)
        if not match:continue
        source=Path(match[1]);name=source.name
        if skip.match(name) or name in seen:continue
        seen.add(name)
        shutil.copy2(source,dest/'usr/lib'/name)
shutil.copy2(root/'packaging/appimage/AppRun',dest/'AppRun');(dest/'AppRun').chmod(0o755)
(dest/'apexsense.desktop').write_text('[Desktop Entry]\nType=Application\nName=ApexSenseBridge\nComment=Unified Flydigi configuration and DualSense bridge\nExec=apexsense\nIcon=apexsense\nCategories=Game;\nTerminal=false\n')
Image.open(root/'gui/assets/logo.png').convert('RGBA').resize((256,256)).save(dest/'apexsense.png')
os.symlink('apexsense.png',dest/'.DirIcon')
print('AppDir ready:',dest,'bundled libraries:',len(seen))

copytree(root/'packaging/appimage/metainfo',dest/'usr/share/metainfo')

subprocess.run(['dpkg-query','-W','-f=${binary:Package}\t${Version}\n'],
    stdout=(dest/'usr/share/ApexSense/BUNDLED-PACKAGES.tsv').open('w'),check=True)
