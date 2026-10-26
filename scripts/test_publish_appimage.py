#!/usr/bin/env python3
"""Exercise release orchestration with a fake gh; never contact GitHub."""
import hashlib,json,os,subprocess,tempfile
from pathlib import Path
SCRIPT=Path(__file__).with_name('publish-appimage.sh').resolve()
SHA='a'*40
with tempfile.TemporaryDirectory() as folder:
    root=Path(folder);binary=root/'bin';binary.mkdir();assets=root/'dist';assets.mkdir()
    payload=b'fake appimage for orchestration tests'
    (assets/'test.AppImage').write_bytes(payload)
    (assets/'SHA256SUMS').write_text(hashlib.sha256(payload).hexdigest()+'  test.AppImage\n')
    fake=binary/'gh'
    fake.write_text('''#!/usr/bin/env python3
import json,os,sys
args=sys.argv[1:]
with open(os.environ['GH_CALLS'],'a') as log:log.write(json.dumps(args)+'\\n')
if args[:2]==['release','view']:sys.exit(0 if os.environ['GH_EXISTS']=='1' else 1)
if args[0]=='api' and '/commits/main' in args[1]:print(os.environ['GH_HEAD'])
if args[:2]==['release','upload'] and os.environ.get('GH_FAIL_UPLOAD')=='1':sys.exit(9)
''');fake.chmod(0o755)
    def run(ref,exists=False,head=SHA,fail=False):
        calls=root/'calls';calls.write_text('')
        env=os.environ.copy();env.update(PATH=str(binary)+os.pathsep+env['PATH'],
            GH_CALLS=str(calls),GH_EXISTS=str(int(exists)),GH_HEAD=head,
            GH_FAIL_UPLOAD=str(int(fail)),GITHUB_REPOSITORY='example/project',
            GITHUB_REF=ref,GITHUB_SHA=SHA)
        result=subprocess.run(['bash',str(SCRIPT),str(assets)],env=env,capture_output=True,text=True)
        return result,[json.loads(line) for line in calls.read_text().splitlines()]
    result,calls=run('refs/heads/main')
    assert result.returncode==0,result.stderr
    created=next(c for c in calls if c[:2]==['release','create'])
    assert created[2]=='preview' and '--prerelease' in created and SHA in created
    result,calls=run('refs/heads/main',exists=True)
    assert result.returncode==0,result.stderr
    assert any(c[:2]==['release','upload'] and '--clobber' in c for c in calls)
    patches=[c for c in calls if '--method' in c and 'PATCH' in c]
    assert len(patches)==1 and 'repos/example/project/git/refs/tags/preview' in patches[0]
    result,calls=run('refs/heads/main',head='b'*40)
    assert result.returncode==0 and len(calls)==1,'Stale run tried to publish'
    result,calls=run('refs/tags/v0.1.2')
    assert result.returncode==0
    assert any(c[:3]==['release','create','v0.1.2'] for c in calls)
    assert not any('PATCH' in c for c in calls),'Immutable version tag was moved'
    result,calls=run('refs/tags/v0.1.2',exists=True)
    assert result.returncode==0 and any(c[:3]==['release','upload','v0.1.2'] for c in calls)
    result,calls=run('refs/heads/main',exists=True,fail=True)
    assert result.returncode!=0 and not any('PATCH' in c for c in calls),'Failed upload moved the preview tag'
    result,calls=run('refs/pull/1/merge')
    assert result.returncode!=0 and not calls,'PR was allowed to publish'
    (assets/'SHA256SUMS').write_text('0'*64+'  test.AppImage\n')
    result,calls=run('refs/heads/main')
    assert result.returncode!=0 and not calls,'Bad artifact checksum was published'
print('PASS: create/update preview, stale-run guard, version tags, upload failure, PR refusal, checksums')
