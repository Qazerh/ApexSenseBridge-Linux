"""Unified UI lifecycle test using a simulated pad, never real HID writes."""
import sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import QThread,QTimer,QProcess
from device_session import DeviceSession,SOURCE
from unified_ui import SECTIONS,apply_palette
sys.path.insert(0,str(SOURCE))
from tests.qml_harness import TestPad

app=QApplication([])
apply_palette()
def wait_for(condition,timeout=10):
 deadline=time.monotonic()+timeout
 while not condition() and time.monotonic()<deadline:
  app.processEvents();QThread.msleep(10)
 assert condition(),'operation timed out'

session=DeviceSession(None)
try:
 session.resume(poll=False)
 backend=session.backend
 pad=TestPad()
 def attach():
  worker=backend.thread.worker
  worker._ctrl=pad
  worker._controller=lambda:pad
 attach()
 backend.requestInfo.emit()
 wait_for(lambda:backend.device.connected)
 backend.profile.select(0)
 wait_for(lambda:backend.profile.loaded)
 backend.profile.title='Draft 1'
 assert backend.profile.dirty
 errors=[]
 session.quick.engine().warnings.connect(lambda warnings:errors.extend(e.toString() for e in warnings))
 for _,filename in SECTIONS:
  if filename:
   session.open_page(filename)
   for _ in range(4):app.processEvents();QThread.msleep(10)
 assert not errors,errors
 assert session.suspend()
 assert backend.thread is None
 assert backend.profile.title=='Draft 1' and backend.profile.dirty
 session.resume(poll=False)
 attach()
 assert backend.profile.title=='Draft 1' and backend.profile.dirty
 backend.profile.write(False)
 wait_for(lambda:session.pending_writes==0)
 assert backend.profile.saveNeeded and not backend.profile.dirty
 assert session.suspend()
 session.resume(poll=False)
 attach()
 assert backend.profile.saveNeeded
 backend.profile.write(True)
 wait_for(lambda:session.pending_writes==0)
 assert not backend.profile.saveNeeded
 assert session.suspend()
 print('PASS: 12 pages; draft survives suspend/resume; apply and flash save; handles released')
finally:session.suspend()
