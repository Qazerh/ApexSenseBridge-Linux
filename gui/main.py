#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
import signal
import sys
from pathlib import Path
from PySide6.QtCore import QProcess, QSettings, QTimer, QUrl
from PySide6.QtGui import QAction, QIcon
from PySide6.QtNetwork import QLocalServer, QLocalSocket
from PySide6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
    QHBoxLayout, QLabel, QPushButton, QFormLayout, QSpinBox, QComboBox,
    QCheckBox, QPlainTextEdit, QSystemTrayIcon, QMenu, QMessageBox)

from runtime import ENGINE, VERSION
BINARY = ENGINE

class Window(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('ApexSenseBridge')
        self.resize(680, 600)
        self.settings = QSettings('ApexSenseBridge', 'GUI')
        self.proc = QProcess(self)
        self.probe = QProcess(self)
        self.proc.setProcessChannelMode(QProcess.MergedChannels)
        self.proc.readyReadStandardOutput.connect(self.read_log)
        self.proc.finished.connect(self.finished)
        self.proc.errorOccurred.connect(self.process_error)
        self.probe.finished.connect(self.probed)
        self.stopping = False
        self.quitting = False
        self.external = False
        self.device_paths = []
        self.hardware = None
        from unified_ui import build_ui
        build_ui(self)
        self.tray = QSystemTrayIcon(self.windowIcon(), self)
        self.tray.setToolTip('ApexSenseBridge')
        menu = QMenu(self)
        show = menu.addAction('Открыть')
        show.triggered.connect(self.show_window)
        menu.addAction('Запустить мост', self.start)
        menu.addAction('Остановить мост', self.stop)
        menu.addSeparator()
        menu.addAction('Выход', self.quit_app)
        self.tray.setContextMenu(menu)
        self.tray.activated.connect(lambda reason: self.show_window() if reason == QSystemTrayIcon.Trigger else None)
        if QSystemTrayIcon.isSystemTrayAvailable():
            self.tray.show()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(4000)
        self.refresh()

    def spin(self, form, label, key, default, maximum):
        spin = QSpinBox()
        spin.setRange(0, maximum)
        spin.setValue(self.settings.value(key, default, type=int))
        form.addRow(label, spin)
        return spin

    def save(self):
        for key, widget in [('strength', self.strength), ('rumble', self.rumble), ('gyro', self.gyro)]:
            self.settings.setValue(key, widget.value())
        self.settings.setValue('touchpad', self.touchpad.currentData())
        self.settings.setValue('lightbar', self.lightbar.isChecked())
        self.settings.sync()

    def running(self):
        return self.proc.state() != QProcess.NotRunning

    def external_pids(self):
        result = []
        own = self.proc.processId()
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit() or int(entry.name) == own:
                continue
            try:
                if entry.stat().st_uid != os.getuid():
                    continue
                if (entry / 'comm').read_text().strip() != 'ApexSenseBridge':
                    continue
                args = (entry / 'cmdline').read_bytes().split(b'\0')
                if len(args) > 1 and args[1] not in (b'', b'bridge', b'bridge-triggers') and not args[1].startswith(b'--'):
                    continue
                result.append(int(entry.name))
            except (OSError, ValueError):
                pass
        return result

    def refresh(self):
        if self.quitting:
            return
        self.external = bool(self.external_pids())
        busy = self.running() or self.external
        from theme import update_badge
        update_badge(self)
        testing = self.console.test.state() != QProcess.NotRunning
        session = self.workspace.session if hasattr(self, 'workspace') else None
        self.start_btn.setEnabled(not busy and not testing and not (session and session.busy()))
        self.stop_btn.setEnabled(busy and not self.stopping)
        self.options.setEnabled(not busy and not testing)
        self.lightbar.setEnabled(not busy)
        if session and session.active:
            self.status.setText('Настройка контроллера • изменения сохраняются при переходах по меню')
            return
        if busy:
            self.status.setText('Остановка моста…' if self.stopping else
                ('Мост запущен в другом окне. Можно остановить его здесь.' if self.external and not self.running()
                 else 'Мост работает • окно можно закрыть — приложение останется в трее'))
            return
        if self.probe.state() == QProcess.NotRunning:
            self.probe.start(BINARY, ['diagnose', '--json'])

    def probed(self, code, status):
        if self.running() or self.external or self.quitting or (hasattr(self, 'workspace') and self.workspace.session.active):
            return
        try:
            data = json.loads(bytes(self.probe.readAllStandardOutput()))
            paths = [d['path'] for d in data.get('devices', [])]
            if paths != self.device_paths:
                selected = self.device.currentData()
                self.device.clear()
                self.device.addItem('Автоматически', '')
                for path in paths:
                    self.device.addItem(path, path)
                self.device.setCurrentIndex(max(0, self.device.findData(selected)))
                self.device_paths = paths
            if not data.get('uhid_read_write'):
                message = 'Нет доступа к UHID. Проверьте системную настройку и переподключите контроллер.'
            elif not data.get('hid_playstation_registered'):
                message = 'Не загружен модуль hid-playstation.'
            elif not paths:
                message = 'Контроллер не найден. Включите его и подключите USB или приёмник 2,4 ГГц.'
            else:
                message = f'Готов к запуску • найдено устройств: {len(paths)}'
            self.status.setText(message)
        except (ValueError, KeyError):
            self.status.setText('Не удалось прочитать диагностику ApexSenseBridge.')

    def open_hardware(self):
        self.workspace.nav.setCurrentRow(3)

    def start(self):
        if self.running() or self.external_pids() or self.console.test.state() != QProcess.NotRunning:
            self.refresh()
            return
        if not self.workspace.prepare_bridge():
            return
        self.save()
        args = ['bridge', '--strength', str(self.strength.value()),
            '--rumble-strength', str(self.rumble.value()), '--gyro-strength', str(self.gyro.value()),
            '--touchpad-profile', self.touchpad.currentData()]
        if self.device.currentData():
            args += ['--device', self.device.currentData()]
        if not self.lightbar.isChecked():
            args.append('--no-lightbar')
        self.log.appendPlainText('Запуск моста…')
        self.proc.start(BINARY, args)
        self.stopping = False
        self.refresh()

    def read_log(self):
        text = bytes(self.proc.readAllStandardOutput()).decode(errors='replace')
        if text:
            self.log.appendPlainText(text.rstrip())

    def stop(self):
        pids = [int(self.proc.processId())] if self.running() else self.external_pids()
        for pid in pids:
            try:
                os.kill(pid, signal.SIGINT)
            except ProcessLookupError:
                pass
        self.stopping = bool(pids)
        self.stop_btn.setEnabled(False)
        self.status.setText('Остановка моста и восстановление настроек контроллера…')
        QTimer.singleShot(1500, self.check_stop)

    def check_stop(self):
        if not self.running() and not self.external_pids():
            self.stopping = False
            if self.quitting:
                QApplication.quit()
            else:
                self.refresh()
        else:
            QTimer.singleShot(1000, self.check_stop)

    def finished(self, code, status):
        self.read_log()
        self.log.appendPlainText(f'Мост остановлен (код {code}).')
        self.stopping = False
        if self.quitting:
            QApplication.quit()
        else:
            self.refresh()

    def process_error(self, error):
        self.log.appendPlainText('Ошибка запуска: ' + self.proc.errorString())
        self.refresh()

    def show_window(self):
        self.showNormal()
        self.raise_()
        self.activateWindow()

    def closeEvent(self, event):
        self.save()
        if QSystemTrayIcon.isSystemTrayAvailable():
            self.hide()
            event.ignore()
            if not self.settings.value('tray_tip', False, type=bool):
                self.tray.showMessage('ApexSenseBridge', 'Приложение осталось в трее. Для остановки используйте меню значка.')
                self.settings.setValue('tray_tip', True)
        else:
            event.ignore()
            self.quit_app()

    def quit_app(self):
        session = self.workspace.session
        if session.busy():
            self.status.setText('Дождитесь завершения записи в контроллер перед выходом.')
            return
        if session.dirty():
            answer = QMessageBox.question(self, 'Несохранённые изменения',
                'Есть несохранённые настройки контроллера. Выйти без их сохранения?',
                QMessageBox.Discard | QMessageBox.Cancel, QMessageBox.Cancel)
            if answer != QMessageBox.Discard:
                return
        if not session.suspend():
            return
        if session.quick is not None:
            session.quick.setSource(QUrl())
        self.save()
        self.quitting = True
        self.timer.stop()
        if self.console.test.state() != QProcess.NotRunning:
            os.kill(int(self.console.test.processId()), signal.SIGINT)
            self.console.test.waitForFinished(3000)
        if self.probe.state() != QProcess.NotRunning:
            self.probe.terminate()
            self.probe.waitForFinished(1000)
        if self.running() or self.external_pids():
            self.stop()
        else:
            QApplication.quit()


def smoke_test(app):
    from unified_ui import SECTIONS
    window = Window()
    window.timer.stop()
    if window.probe.state() != QProcess.NotRunning:
        window.probe.terminate()
        window.probe.waitForFinished(2000)
    session = window.workspace.session
    session.initialize()
    warnings = []
    session.quick.engine().warnings.connect(lambda items: warnings.extend(e.toString() for e in items))
    for _, filename in SECTIONS:
        if filename:
            session.open_page(filename)
            for _ in range(6):
                app.processEvents()
    session.shutdown()
    session.quick.setSource(QUrl())
    app.processEvents()
    if warnings:
        print('\n'.join(warnings), file=sys.stderr)
        return 1
    print('ApexSenseBridge GUI smoke test passed (12 device pages; no HID writes).')
    return 0


def main():
    if '--visual-test' in sys.argv:
        import runpy
        runpy.run_path(str(Path(__file__).parent/'tests/test_visual_layout.py'), run_name='__main__')
        return 0
    if '--version' in sys.argv:
        print('ApexSenseBridge Linux GUI ' + VERSION)
        return 0
    if '--install-device-rules' in sys.argv:
        import tempfile, shutil, subprocess
        from runtime import ROOT
        with tempfile.TemporaryDirectory(prefix='apexsense-device-setup-') as folder:
            stage = Path(folder)
            shutil.copy2(ROOT/'setup-devices.sh', stage/'setup-devices.sh')
            (stage/'packaging/linux').mkdir(parents=True)
            for name in ['70-apexsensebridge.rules','71-apexsensebridge-flydigi.rules']:
                shutil.copy2(ROOT/'packaging/linux'/name, stage/'packaging/linux'/name)
            return subprocess.run(['pkexec','/bin/sh',str(stage/'setup-devices.sh')]).returncode
    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)
    app.setApplicationName('ApexSenseBridge')
    app.setDesktopFileName('apexsensebridge')
    app.setWindowIcon(QIcon.fromTheme('input-gaming'))
    if '--smoke-test' in sys.argv:
        return smoke_test(app)
    name = f'apexsensebridge-gui-{os.getuid()}'
    socket = QLocalSocket()
    socket.connectToServer(name)
    if socket.waitForConnected(400):
        socket.write(b'show')
        socket.waitForBytesWritten(400)
        return 0
    QLocalServer.removeServer(name)
    server = QLocalServer()
    server.setSocketOptions(QLocalServer.UserAccessOption)
    if not server.listen(name):
        return 1
    window = Window()
    def incoming():
        peer = server.nextPendingConnection()
        if peer:
            peer.close()
            peer.deleteLater()
        window.show_window()
    server.newConnection.connect(incoming)
    window.show()
    if '--hardware' in sys.argv:
        QTimer.singleShot(0, window.open_hardware)
    return app.exec()

if __name__ == '__main__':
    sys.exit(main())
