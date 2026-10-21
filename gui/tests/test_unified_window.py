"""Exercise full navigation and mode switching with fake HID and bridge."""
import sys,time,tempfile,signal,os
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from PySide6.QtWidgets import QApplication,QDialog
from PySide6.QtCore import QThread,QProcess
import main
from device_session import DeviceSession,SOURCE
sys.path.insert(0,str(SOURCE))
from tests.qml_harness import TestPad
app=QApplication([])
app.setQuitOnLastWindowClosed(False)
pad=TestPad()
original_resume=DeviceSession.resume

def fake_resume(self,poll=True):
 original_resume(self,poll=False)
 worker=self.backend.thread.worker
 worker._ctrl=pad
 worker._controller=lambda:pad
DeviceSession.resume=fake_resume

def wait_for(condition,timeout=10):
 deadline=time.monotonic()+timeout
 while not condition() and time.monotonic()<deadline:
  app.processEvents();QThread.msleep(10)
 assert condition(),'operation timed out'

with tempfile.TemporaryDirectory() as tmp:
 binary=Path(tmp)/'bridge'
 binary.write_text('''#!/usr/bin/python3
import sys,signal,time,json
if 'diagnose' in sys.argv:
 print(json.dumps({'uhid_read_write':True,'hid_playstation_registered':True,'devices':[{'path':'/dev/test'}]}))
else:
 signal.signal(signal.SIGINT,lambda *_:sys.exit(0))
 print('Bridge running.',flush=True)
 while True:time.sleep(.02)
''')
 binary.chmod(0o755)
 main.BINARY=str(binary)
 w=main.Window()
 w.external_pids=lambda:[]
 try:
  w.workspace.nav.setCurrentRow(3)
  session=w.workspace.session
  backend=session.backend
  backend.requestInfo.emit()
  backend.profile.select(0)
  wait_for(lambda:backend.profile.loaded)
  backend.profile.title='Draft 2'
  w.workspace.nav.setCurrentRow(4)
  assert backend.profile.title=='Draft 2'
  w.start()
  wait_for(lambda:w.proc.state()==QProcess.Running)
  assert not session.active
  assert backend.thread is None
  assert backend.profile.title=='Draft 2' and backend.profile.dirty
  w.workspace.nav.setCurrentRow(4)
  assert w.workspace.stack.currentWidget()==w.workspace.blocked
  w.workspace.stop_then_open()
  wait_for(lambda:not w.running() and session.active)
  assert w.workspace.stack.currentWidget()==session
  assert backend.profile.title=='Draft 2' and backend.profile.dirty
  w.workspace.nav.setCurrentRow(14)
  assert not session.active
  w.workspace.search.setText('warframe')
  assert w.workspace.table.rowCount()==1
  w.workspace.nav.setCurrentRow(15)
  w.console.run_test(['diagnose','--json'])
  wait_for(lambda:w.console.test.state()==QProcess.NotRunning)
  assert 'uhid_read_write' in w.log.toPlainText()
  assert not any(isinstance(widget,QDialog) for widget in app.topLevelWidgets())
  print('PASS: one window, shared navigation, draft preservation, exclusive bridge/config ownership, game search, diagnostics')
 finally:
  w.timer.stop()
  if w.probe.state()!=QProcess.NotRunning:
   w.probe.terminate();w.probe.waitForFinished(2000)
  if w.running():
   os.kill(int(w.proc.processId()),signal.SIGINT)
   w.proc.waitForFinished(3000)
  if w.console.test.state()!=QProcess.NotRunning:
   w.console.test.terminate();w.console.test.waitForFinished(1000)
  w.workspace.session.pending_writes=0
  w.workspace.session.suspend()
