#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rendered geometry and pointer-input regressions, with a fake pad and dock."""
import argparse,json,math,os,sys,tempfile,time,traceback
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('--output-dir',type=Path,default=Path('dist/visual-tests'))
parser.add_argument('--visual-test',action='store_true')
args=parser.parse_args()
args.output_dir.mkdir(parents=True,exist_ok=True)
fixture=tempfile.TemporaryDirectory(prefix='apexsense-visual-')
stage=Path(fixture.name)
os.environ['XDG_CONFIG_HOME']=str(stage/'config')
os.environ['XDG_CACHE_HOME']=str(stage/'cache')
os.environ.setdefault('QT_QPA_PLATFORM','offscreen')
os.environ.setdefault('QT_QUICK_BACKEND','software')
bus=stage/'bus.json'
bus.write_text(json.dumps({'hide_real':True,'devices':[
    {'kind':'pad','code':'k5','nickname':'Test controller'},
    {'kind':'dock','type':0,'nickname':'Test charging dock'}]}))
os.environ['FLYDIGI_MOCK_BUS']=str(bus)
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from PySide6.QtCore import QPointF,QThread,QUrl,QProcess,qVersion
from PySide6.QtQml import QQmlExpression
from PySide6.QtWidgets import QApplication
from PySide6.QtTest import QTest
from PySide6.QtCore import Qt
from PySide6.QtGui import QImage,QColor
import main
from unified_ui import SECTIONS
probe=stage/'probe'
probe.write_text('#!/bin/sh\nprintf \'%s\\n\' \'{"uhid_read_write":true,"hid_playstation_registered":true,"devices":[]}\'\n')
probe.chmod(0o755)
main.BINARY=str(probe)
main.Window.external_pids=lambda self:[]
app=QApplication([])
app.setQuitOnLastWindowClosed(False)
window=main.Window()
window.show()
window.workspace.nav.setCurrentRow(13)
session=window.workspace.session
errors=[]
report={'qt':qVersion(),'scale':os.environ.get('QT_SCALE_FACTOR','1'),'cases':[]}


def settle(milliseconds=150):
    QTest.qWait(milliseconds)


def wait_for(predicate,timeout=10):
    deadline=time.monotonic()+timeout
    while not predicate() and time.monotonic()<deadline:settle(20)
    assert predicate(),'Timed out waiting for simulated devices/UI'


def evaluate(script):
    root=session.quick.rootObject()
    expression=QQmlExpression(session.quick.engine().rootContext(),root,script)
    result=expression.evaluate()
    assert not expression.hasError(),expression.error().toString()
    return result[0] if isinstance(result,tuple) else result


INSPECT = r"""(function() {
    var root=this, failures=[], controls=0, switches=0, target=null;
    function walk(item,fn) {
        fn(item);
        var children=item.children || [];
        for (var i=0;i<children.length;i++) walk(children[i],fn);
    }
    function cardFor(item) {
        var p=item.parent;
        while(p) {
            if(p.maximumWidth!==undefined && p.cardWidthRestricted!==undefined)return p;
            p=p.parent;
        }
        return null;
    }
    walk(root,function(item) {
        if(!item.visible)return;
        if(item.contentItem!==undefined && item.padding!==undefined) {
            var card=cardFor(item);
            if(card) {
                var pos=item.mapToItem(card,0,0), inset=Math.max(0,(card.width-card.maximumWidth)/2);
                if(pos.x<inset-2 || pos.x+item.width>card.width-inset+2)
                    failures.push((item.objectName || item.toString())+': control spills outside card');
                controls++;
            }
        }
        if(item.checked===undefined || item.position===undefined || !item.indicator)return;
        switches++;
        var indicator=item.indicator, point=indicator.mapToItem(item,0,0);
        if(item.width<indicator.width-1)
            failures.push('Switch hit area is narrower than visible track: '+item.width+' < '+indicator.width);
        if(point.x < -1 || point.y < -1 || point.x+indicator.width>item.width+1 || point.y+indicator.height>item.height+1)
            failures.push('Switch track extends beyond its hit area');
        var pieces=indicator.children || [];
        for(var i=0;i<pieces.length;i++) {
            var piece=pieces[i], local=piece.mapToItem(indicator,0,0);
            if(piece.visible && (local.x < -1 || local.y < -1 || local.x+piece.width>indicator.width+1 || local.y+piece.height>indicator.height+1))
                failures.push('Switch thumb extends beyond the track');
        }
        var row=item.parent;
        while(row && row.description===undefined)row=row.parent;
        if(row) {
            var trackLeft=indicator.mapToItem(row,0,0).x;
            walk(row,function(text) {
                if(!text.visible || text.paintedWidth===undefined)return;
                if(text.text!==row.text && text.text!==row.description)return;
                if(text.mapToItem(row,0,0).x+text.width>trackLeft-8)
                    failures.push('Switch overlaps its title/description');
                if(text.paintedWidth>text.width+2)failures.push('Description is horizontally clipped');
                if(text.paintedHeight>text.height+2)failures.push('Description is vertically clipped');
            });
            if(row.objectName==='dockSleepWhenCharging') {
                var centre=item.mapToItem(root,item.width/2,item.height/2);
                var rowPoint=row.mapToItem(root,24,row.height/2);
                target={x:centre.x,y:centre.y,rowX:rowPoint.x,rowY:rowPoint.y,checked:item.checked};
            }
        }
    });
    return JSON.stringify({controls:controls,switches:switches,errors:failures,target:target});
}).call(this)"""


def inspect_layout():
    return json.loads(evaluate(INSPECT))


def click_dock_switch():
    info=inspect_layout()
    target=info['target']
    assert target is not None,'Dock switch was not rendered'
    original=session.backend.dock.sleepWhenCharging
    QTest.mouseClick(session.quick,Qt.LeftButton,Qt.NoModifier,QPointF(target['x'],target['y']).toPoint())
    wait_for(lambda:session.backend.dock.sleepWhenCharging!=original and not session.pending_writes)
    session.backend.dock.sleepWhenCharging=original
    wait_for(lambda:inspect_layout()['target']['checked']==original and not session.pending_writes)
    settle(120)  # let the conflict hint and row positions finish relayout
    target=inspect_layout()['target']
    QTest.mouseClick(session.quick,Qt.LeftButton,Qt.NoModifier,QPointF(target['rowX'],target['rowY']).toPoint())
    wait_for(lambda:session.backend.dock.sleepWhenCharging!=original and not session.pending_writes)
    session.backend.dock.sleepWhenCharging=original
    wait_for(lambda:inspect_layout()['target']['checked']==original and not session.pending_writes)
    QTest.keyClick(session.quick,Qt.Key_Space)
    wait_for(lambda:session.backend.dock.sleepWhenCharging!=original and not session.pending_writes)
    session.backend.dock.sleepWhenCharging=original
    wait_for(lambda:inspect_layout()['target']['checked']==original and not session.pending_writes)


try:
    assert session.backend is not None, window.log.toPlainText()
    wait_for(lambda:session.backend.dock.present and session.backend.profile.loaded)
    session.quick.engine().warnings.connect(lambda warnings:errors.extend(e.toString() for e in warnings))
    picture=stage/'test-picture.png'
    image=QImage(320,160,QImage.Format_RGB32)
    image.fill(QColor('#1E6FE8'));assert image.save(str(picture))
    session.backend.dock.modeIndex=session.backend.dock.modeNames.index('Picture')
    session.backend.dock.openImage(QUrl.fromLocalFile(str(picture)))
    session.backend.screen.open(QUrl.fromLocalFile(str(picture)))
    wait_for(lambda:session.backend.dock.hasImage and bool(session.backend.screen.previewSource) and not session.backend.screen.encoding)

    for width,height in [(1060,740),(1280,880),(1600,1000)]:
        window.resize(width,height)
        for index,(title,filename) in enumerate(SECTIONS):
            if not filename and index not in (0,1,14,15):continue
            case={'page':title,'size':[width,height]}
            errors.clear()
            try:
                window.workspace.nav.setCurrentRow(index)
                settle()
                if filename:
                    info=inspect_layout()
                    case['controls']=info['controls']
                    case['switches']=info['switches']
                    assert not info['errors'], '; '.join(info['errors'][:6])
                    if index==13:
                        assert case['switches']>=4,'Dock fixture did not expose all switches'
                        click_dock_switch()
                        settle()
                else:
                    assert window.workspace.stack.currentWidget() is window.workspace.pages[index]
                assert not errors,errors
                case['status']='passed'
            except Exception as exc:
                case['status']='failed';case['error']=str(exc)
                print(f'FAIL {width}x{height} {title}: {exc}',flush=True)
            image=args.output_dir/f'{width}x{height}-{index:02d}.png'
            window.grab().save(str(image))
            if index in (11,13):
                evaluate("(function visit(item) { if(item.flickable && item.flickable.contentHeight!==undefined) item.flickable.contentY=Math.max(0,item.flickable.contentHeight-item.flickable.height); var children=item.children||[]; for(var i=0;i<children.length;i++)visit(children[i]); })(this)")
                settle()
                window.grab().save(str(args.output_dir/f'{width}x{height}-{index:02d}-bottom.png'))
            report['cases'].append(case)
    (args.output_dir/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    failures=[case for case in report['cases'] if case['status']=='failed']
    print(f'Visual layout: {len(report["cases"])-len(failures)}/{len(report["cases"])} passed; Qt {qVersion()}, scale {report["scale"]}',flush=True)
    if failures:raise SystemExit(1)
except Exception:
    report['startup_error']=traceback.format_exc()
    (args.output_dir/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    window.grab().save(str(args.output_dir/'failure.png'))
    raise
finally:
    window.timer.stop()
    if window.probe.state()!=QProcess.NotRunning:
        window.probe.terminate();window.probe.waitForFinished(2000)
    session.pending_writes=0
    session.suspend()
    if session.quick is not None:session.quick.setSource(QUrl())
    app.processEvents()
    fixture.cleanup()
