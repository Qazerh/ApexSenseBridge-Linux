# SPDX-License-Identifier: GPL-3.0-or-later
"""Persistent Flydigi models with exclusive, suspendable access to hardware."""
from pathlib import Path
import os
import sys
from PySide6.QtCore import QUrl, QMetaObject, Qt, Q_ARG, Signal
from PySide6.QtWidgets import QWidget, QVBoxLayout
SOURCE = Path(__file__).resolve().parent.parent / 'vendor/openflydigi'

class DeviceSession(QWidget):
    changed = Signal()
    statusMessage = Signal(str)

    def __init__(self,owner):
        super().__init__(owner)
        self.backend=None
        self.quick=None
        self.active=False
        self.pending_writes=0
        self.requests_connected=False
        self.layout=QVBoxLayout(self)
        self.layout.setContentsMargins(0,0,0,0)

    def initialize(self):
        from PySide6.QtQuickWidgets import QQuickWidget
        from PySide6.QtQml import qmlTypeId
        from PySide6.QtQuickControls2 import QQuickStyle
        if str(SOURCE) not in sys.path:sys.path.insert(0,str(SOURCE))
        from gui import i18n
        from gui.app import App
        QQuickStyle.setStyle('Basic')
        self.quick=QQuickWidget(self)
        self.quick.setResizeMode(QQuickWidget.SizeRootObjectToView)
        engine=self.quick.engine()
        if not os.environ.get('APEXSENSE_BUNDLED'):
            for path in ('/usr/lib/qt6/qml','/usr/lib64/qt6/qml','/usr/lib/qml'):
                if Path(path).is_dir():engine.addImportPath(path)
        engine.rootContext().setContextProperty('apexAssetBase', QUrl.fromLocalFile(str(Path(__file__).parent/'assets')+'/').toString())
        i18n.install(engine)
        self.quick.setSource(QUrl.fromLocalFile(str(SOURCE/'gui/qml/UnifiedDevicePage.qml')))
        if self.quick.status()==QQuickWidget.Error:
            raise RuntimeError('\n'.join(error.toString() for error in self.quick.errors()))
        self.backend=engine.singletonInstance(qmlTypeId('Apex5',1,0,'App'))
        self.backend.device.connectedChanged.connect(self.changed)
        self.backend.device.statusChanged.connect(self.forward_status)
        self.layout.addWidget(self.quick)

    def forward_status(self):
        if self.backend.device.status:
            self.statusMessage.emit(self.backend.device.status)

    def resume(self,poll=True):
        if self.active:return
        if self.quick is None:self.initialize()
        self.backend._was_connected=False
        self.backend.start(poll)
        self.active=True
        self.track_writes()
        self.changed.emit()

    def track_writes(self):
        worker=self.backend.thread.worker
        for request,completed in [
            (self.backend.profile.writeRequested,worker.profile_written),
            (self.backend.profile.resetRequested,worker.profile_loaded),
            (self.backend.lighting.writeRequested,worker.lighting_written),
            (self.backend.settings.writeRequested,worker.setting_written),
            (self.backend.dock.switchRequested,worker.dock_switch),
            (self.backend.dock.lightingRequested,worker.dock_finished),
        ]:
            if not self.requests_connected:request.connect(self.write_started)
            completed.connect(self.write_finished)
        self.requests_connected=True
        worker.failed.connect(self.write_failed)

    def write_started(self,*_):
        self.pending_writes+=1
        self.changed.emit()

    def write_finished(self,*_):
        self.pending_writes=max(0,self.pending_writes-1)
        self.changed.emit()

    write_failed=write_finished
    def busy(self):
        if self.backend is None:return False
        return bool(self.backend.screen.busy or self.backend.dock.busy or
            self.backend.profile.macros.recording or self.pending_writes > 0)

    def dirty(self):
        if self.backend is None:return False
        return bool(self.backend.profile.dirty or self.backend.profile.saveNeeded or
            self.backend.lighting.dirty or self.backend.lighting.saveNeeded or
            self.backend.dock.lightingDirty)

    def open_page(self,filename):
        if self.quick is None:self.initialize()
        QMetaObject.invokeMethod(self.quick.rootObject(),'openPage',Q_ARG('QVariant',filename))

    def suspend(self):
        """Retain edits and model objects, but release all device handles."""
        if not self.active:return True
        if self.busy():return False
        backend=self.backend
        backend._polling=False
        backend._info_timer.stop()
        backend._devices_timer.stop()
        backend.dsmode.polling=False
        thread=backend.thread
        if thread is not None and thread.thread.isRunning():
            QMetaObject.invokeMethod(thread.worker,'shutdown',Qt.BlockingQueuedConnection)
        backend.shutdown()
        if backend.thread is not None:return False
        if thread is not None:
            # stop() has joined the thread and closed the handle. No queued
            # request can use these QObjects now; deleting removes old signal
            # connections before a new worker is attached to the same models.
            import shiboken6
            shiboken6.delete(thread.worker)
            shiboken6.delete(thread.thread)
        self.active=False
        self.changed.emit()
        return True

    def shutdown(self):
        return self.suspend()
