#!/usr/bin/env python3
"""Download official, checksum-pinned packaging tools without FUSE."""
import hashlib,json,subprocess,sys,urllib.request
from pathlib import Path
root=Path(sys.argv[1]);root.mkdir(parents=True,exist_ok=True)
lock=json.loads(Path(__file__).with_name('tools.lock.json').read_text())
for name,entry in lock.items():
    data=urllib.request.urlopen(entry['url'],timeout=120).read()
    if hashlib.sha256(data).hexdigest()!=entry['sha256']:
        raise SystemExit(f'{name}: SHA-256 mismatch; update tools.lock.json from verified upstream metadata')
    target=root/name
    target.write_bytes(data);target.chmod(0o755)
    if name=='appimagetool':
        subprocess.run([str(target),'--appimage-extract'],cwd=root,check=True,stdout=subprocess.DEVNULL)
        target.unlink()
        (root/'squashfs-root').rename(target)
