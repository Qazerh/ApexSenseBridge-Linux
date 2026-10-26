# SPDX-License-Identifier: GPL-3.0-or-later
"""One application shell, navigation and visual theme for both engines."""
import json
from PySide6.QtCore import Qt, QObject, QProcess, QTimer
from PySide6.QtGui import QIcon, QPalette, QColor
from PySide6.QtWidgets import (QApplication,QWidget,QHBoxLayout,QVBoxLayout,QStackedWidget,
 QListWidget,QListWidgetItem,QFrame,QPushButton,QLabel,QFormLayout,QComboBox,
 QSpinBox,QSlider,QCheckBox,QPlainTextEdit,QLineEdit,QTableWidget,QTableWidgetItem,
 QHeaderView,QScrollArea,QMessageBox)
from theme import STYLE,BASE,label,card,art,update_badge
from device_session import DeviceSession
from runtime import VERSION

SECTIONS=[
 ('Обзор',None),('Мост DualSense',None),
 ('Устройства','DevicesPage.qml'),('Профили','ControllerPage.qml'),
 ('Кнопки','ButtonsPage.qml'),('Макросы','MacrosPage.qml'),
 ('Стики','SticksPage.qml'),('Гироскоп','GyroPage.qml'),
 ('Триггеры','TriggersPage.qml'),('Вибрация','VibrationPage.qml'),
 ('Подсветка','LightingPage.qml'),('Экран','ScreenPage.qml'),
 ('Параметры устройства','DeviceSettingsPage.qml'),('Док-станция','DockPage.qml'),
 ('Совместимые игры',None),('Диагностика и журнал',None)]

EXTRA_STYLE='''
QWidget#sidebar { background: #0B1225; border-right: 1px solid #23304A; }
QListWidget { background: transparent; border: none; outline: none; }
QListWidget::item { color: #AAB4CC; padding: 10px 13px; margin: 2px 0; border-radius: 7px; }
QListWidget::item:selected { color: white; background: #1E6FE8; }
QListWidget::item:hover:!selected { background: #1B2540; color: white; }
QStackedWidget { background: transparent; }
QLabel#pageTitle { font-size: 24px; font-weight: 600; }
'''

def apply_palette():
    palette=QPalette()
    colors={'Window':'#0B1225','WindowText':'#F4F6FB','Base':'#111A31',
        'AlternateBase':'#17213A','Text':'#F4F6FB','Button':'#1B2540',
        'ButtonText':'#F4F6FB','Highlight':'#1E6FE8','HighlightedText':'#FFFFFF',
        'ToolTipBase':'#1B2540','ToolTipText':'#F4F6FB','Link':'#8DB7FF',
        'Light':'#3A4768','Midlight':'#23304A','Mid':'#23304A','Dark':'#060A16'}
    for role,color in colors.items():palette.setColor(getattr(QPalette,role),QColor(color))
    palette.setColor(QPalette.Disabled,QPalette.Text,QColor('#6E7995'))
    palette.setColor(QPalette.Disabled,QPalette.ButtonText,QColor('#6E7995'))
    QApplication.instance().setPalette(palette)


def scroll_page():
    scroll=QScrollArea()
    scroll.setWidgetResizable(True)
    body=QWidget()
    layout=QVBoxLayout(body)
    layout.setContentsMargins(0,0,8,0)
    layout.setSpacing(16)
    scroll.setWidget(body)
    return scroll,layout

class Diagnostics(QObject):
    def __init__(self,owner):
        super().__init__(owner)
        self.owner=owner
        self.test=QProcess(self)
        self.test.setProcessChannelMode(QProcess.MergedChannels)
        self.test.readyReadStandardOutput.connect(self.output)
        self.test.finished.connect(self.finished)
        self.test.errorOccurred.connect(self.error)
    def run_test(self,args):
        from main import BINARY
        w=self.owner
        if w.running() or w.external_pids():
            w.log.appendPlainText('Остановите мост перед проверкой.')
            return
        if self.test.state()!=QProcess.NotRunning:return
        if not w.workspace.prepare_bridge(navigate=False):return
        w.log.appendPlainText('Проверка: '+' '.join(args))
        if w.device.currentData():args=args+['--device',w.device.currentData()]
        self.test.start(BINARY,args)
        w.refresh()
    def output(self):
        text=bytes(self.test.readAllStandardOutput()).decode(errors='replace')
        if text:self.owner.log.appendPlainText(text.rstrip())
    def finished(self,*_):
        self.output()
        self.owner.refresh()
    def error(self,*_):
        self.owner.log.appendPlainText(self.test.errorString())
        self.owner.refresh()

class Workspace(QObject):
    def __init__(self,w):
        super().__init__(w)
        self.w=w
        self.current=0
        self.pending=None
        self.session=DeviceSession(w)
        self.session.changed.connect(w.refresh)
        self.pages={}
        root=QWidget()
        root.setObjectName('shell')
        w.setCentralWidget(root)
        shell=QHBoxLayout(root)
        shell.setContentsMargins(0,0,0,0)
        shell.setSpacing(0)
        sidebar=QWidget()
        sidebar.setObjectName('sidebar')
        sidebar.setFixedWidth(240)
        side=QVBoxLayout(sidebar)
        side.setContentsMargins(16,22,16,18)
        brand=QHBoxLayout()
        brand.addWidget(art('logo.png',36,36))
        brand.addWidget(label('ApexSenseBridge','title'),1)
        side.addLayout(brand)
        side.addWidget(label('Центр управления контроллером','muted'))
        self.nav=QListWidget()
        for name,_ in SECTIONS:self.nav.addItem(name)
        side.addWidget(self.nav,1)
        side.addWidget(label(f'v{VERSION} · Flydigi + DualSense','muted'))
        shell.addWidget(sidebar)
        content=QVBoxLayout()
        content.setContentsMargins(28,24,28,20)
        content.setSpacing(16)
        shell.addLayout(content,1)
        header=QHBoxLayout()
        self.title=label('Обзор','pageTitle')
        header.addWidget(self.title,1)
        w.badge=label('ОЖИДАНИЕ','badge')
        header.addWidget(w.badge)
        w.start_btn=QPushButton('Запустить мост')
        w.start_btn.setObjectName('primary')
        w.start_btn.clicked.connect(w.start)
        w.stop_btn=QPushButton('Остановить')
        w.stop_btn.clicked.connect(w.stop)
        header.addWidget(w.start_btn)
        header.addWidget(w.stop_btn)
        content.addLayout(header)
        w.status=label('Проверка контроллера…','muted')
        content.addWidget(w.status)
        self.stack=QStackedWidget()
        content.addWidget(self.stack,1)
        self.build_overview()
        self.build_bridge()
        self.stack.addWidget(self.session)
        self.build_games()
        self.build_log()
        self.blocked,self.blocked_layout=scroll_page()
        notice=card(self.blocked_layout)
        notice.addWidget(label('Контроллер используется мостом','title'))
        notice.addWidget(label('Для настройки профилей и устройства нужно сначала остановить мост.'))
        stop=QPushButton('Остановить мост и открыть настройки')
        stop.setObjectName('primary')
        stop.clicked.connect(self.stop_then_open)
        notice.addWidget(stop)
        self.blocked_layout.addStretch()
        self.stack.addWidget(self.blocked)
        self.nav.currentRowChanged.connect(self.navigate)
        self.nav.setCurrentRow(0)

    def add_page(self,index,widget):
        self.pages[index]=widget
        self.stack.addWidget(widget)

    def build_overview(self):
        page,layout=scroll_page()
        hero=card(layout)
        row=QHBoxLayout()
        left=QVBoxLayout()
        left.addWidget(label('Ваш Flydigi.\nВозможности DualSense.','pageTitle'))
        left.addWidget(label('Профили контроллера и игровой мост — в одной панели.','muted'))
        left.addSpacing(14)
        pills=QHBoxLayout()
        for text in ['Адаптивные триггеры','Вибрация','Гироскоп']:pills.addWidget(label(text,'pill'))
        pills.addStretch()
        left.addLayout(pills)
        left.addStretch()
        row.addLayout(left,1)
        row.addWidget(art('controller-apex5.png',280,230))
        hero.addLayout(row)
        tiles=QHBoxLayout()
        for title,description,target in [
            ('Профили и кнопки','Переназначения, макросы и сохранение в контроллер.',3),
            ('Эффекты в играх','Триггеры, вибрация, гироскоп и жесты тачпада.',1),
            ('Внешний вид','Подсветка и изображение на экране Apex 5.',10)]:
            frame=QFrame();frame.setObjectName('card')
            box=QVBoxLayout(frame)
            box.setContentsMargins(18,18,18,18)
            box.addWidget(label(title,'title'))
            box.addWidget(label(description,'muted'),1)
            button=QPushButton('Открыть')
            button.clicked.connect(lambda checked=False,target=target:self.nav.setCurrentRow(target))
            box.addWidget(button)
            tiles.addWidget(frame,1)
        layout.addLayout(tiles)
        tip=card(layout)
        tip.addWidget(label('Перед игрой'))
        tip.addWidget(label('Отключите Steam Input в свойствах игры. Настройки устройства доступны при остановленном мосте; черновики сохраняются при переходах по меню.','muted'))
        layout.addStretch()
        self.add_page(0,page)

    def build_bridge(self):
        w=self.w
        page,layout=scroll_page()
        effect=card(layout)
        effect.addWidget(label('Игровой мост DualSense','title'))
        effect.addWidget(label('Эти параметры управляют переводом эффектов игры. Встроенные настройки самого контроллера находятся в соседних разделах меню.','muted'))
        w.options=QWidget()
        form=QFormLayout(w.options)
        form.setVerticalSpacing(24)
        w.device=QComboBox();w.device.addItem('Автоматически','')
        form.addRow('Контроллер',w.device)
        for attr,title,maximum in [('strength','Сила адаптивных триггеров',100),('rumble','Сила вибрации',200),('gyro','Чувствительность гироскопа',400)]:
            line=QWidget();bar=QHBoxLayout(line);bar.setContentsMargins(0,0,0,0)
            slider=QSlider(Qt.Horizontal);slider.setRange(0,maximum)
            spin=QSpinBox();spin.setRange(0,maximum);spin.setSuffix(' %')
            spin.setValue(w.settings.value(attr,100,type=int));slider.setValue(spin.value())
            spin.valueChanged.connect(slider.setValue);slider.valueChanged.connect(spin.setValue)
            bar.addWidget(slider,1);bar.addWidget(spin)
            setattr(w,attr,spin);form.addRow(title,line)
        w.touchpad=QComboBox()
        for title,key in [('Без жестов','none'),('View → свайп вверх','legacy-view-hold-swipe-up'),('Spider-Man 2','spider-man-2'),('Miles Morales','miles-morales'),('Ghost of Tsushima','ghost-of-tsushima'),('Warframe','warframe'),('Death Stranding 2','death-stranding-2')]:w.touchpad.addItem(title,key)
        w.touchpad.setCurrentIndex(max(0,w.touchpad.findData(w.settings.value('touchpad','none'))))
        form.addRow('Профиль тачпада',w.touchpad)
        w.lightbar=QCheckBox('Синхронизировать подсветку с игрой')
        w.lightbar.setChecked(w.settings.value('lightbar',True,type=bool))
        form.addRow(w.lightbar)
        effect.addWidget(w.options)
        save=QPushButton('Сохранить параметры моста');save.setObjectName('primary')
        save.clicked.connect(w.save);effect.addWidget(save,0,Qt.AlignRight)
        note=card(layout)
        note.addWidget(label('Настройки применяются при следующем запуске моста. Linux-мост не поддерживает PCM-аудиоотдачу DualSense.','muted'))
        layout.addStretch()
        self.add_page(1,page)

    def build_games(self):
        page=QWidget();layout=QVBoxLayout(page)
        layout.setContentsMargins(0,0,0,0)
        catalogue=json.loads((BASE/'supported_games.json').read_text())
        self.games=catalogue['games']
        self.search=QLineEdit();self.search.setPlaceholderText(f'Поиск среди {len(self.games)} игр…')
        self.table=QTableWidget(0,3)
        self.table.setHorizontalHeaderLabels(['Игра','Адаптивные триггеры','Haptics в Windows'])
        self.table.horizontalHeader().setSectionResizeMode(0,QHeaderView.Stretch)
        for i in [1,2]:self.table.horizontalHeader().setSectionResizeMode(i,QHeaderView.ResizeToContents)
        self.table.verticalHeader().hide();self.table.setEditTriggers(QTableWidget.NoEditTriggers)
        self.table.setSelectionBehavior(QTableWidget.SelectRows);self.table.setShowGrid(False)
        self.search.textChanged.connect(self.filter_games)
        layout.addWidget(self.search);layout.addWidget(self.table,1)
        layout.addWidget(label('Каталог оригинального проекта. Нативные эффекты зависят от игры и Proton; PCM/audio haptics в Linux-мосте недоступны.','muted'))
        self.filter_games('');self.add_page(14,page)

    def filter_games(self,text):
        games=[g for g in self.games if text.casefold() in g['title'].casefold()]
        self.table.setRowCount(len(games))
        for row,g in enumerate(games):
            for col,text in enumerate([g['title'],'Да' if g.get('adaptiveTriggers') else '—','Да' if g.get('hapticFeedback') else '—']):self.table.setItem(row,col,QTableWidgetItem(text))

    def build_log(self):
        w=self.w
        page=QWidget();layout=QVBoxLayout(page);layout.setContentsMargins(0,0,0,0)
        buttons=QHBoxLayout()
        for title,args in [('Доступ к устройствам',['diagnose','--json']),('Модель и батарея',['identify','--json']),('Триггеры · 2 сек',['test-triggers','--seconds','2']),('Вибрация · 2 сек',['test-rumble','--seconds','2'])]:
            button=QPushButton(title)
            button.clicked.connect(lambda checked=False,args=args:w.console.run_test(args))
            buttons.addWidget(button)
        layout.addLayout(buttons)
        w.log=QPlainTextEdit();w.log.setReadOnly(True);w.log.setMaximumBlockCount(1500)
        layout.addWidget(w.log,1)
        footer=QHBoxLayout()
        w.refresh_btn=QPushButton('Обновить состояние');w.refresh_btn.clicked.connect(w.refresh)
        footer.addWidget(w.refresh_btn);footer.addStretch()
        clear=QPushButton('Очистить журнал');clear.clicked.connect(w.log.clear);footer.addWidget(clear)
        layout.addLayout(footer)
        self.add_page(15,page)

    def navigate(self,index):
        if index<0:return
        w=self.w
        filename=SECTIONS[index][1]
        if not filename and not self.session.suspend():
            self.restore_navigation();return
        self.current=index
        self.title.setText(SECTIONS[index][0])
        if filename:
            if w.running() or w.external_pids() or w.console.test.state()!=QProcess.NotRunning:
                self.pending=index
                self.stack.setCurrentWidget(self.blocked)
                return
            try:
                self.session.resume()
                self.session.open_page(filename)
                self.stack.setCurrentWidget(self.session)
            except Exception as exc:
                w.log.appendPlainText('Ошибка настройки устройства: '+str(exc))
                self.stack.setCurrentWidget(self.pages[15])
        else:self.stack.setCurrentWidget(self.pages[index])
        w.refresh()

    def restore_navigation(self):
        self.nav.blockSignals(True);self.nav.setCurrentRow(self.current);self.nav.blockSignals(False)
        self.w.status.setText('Дождитесь завершения записи в контроллер перед сменой режима.')

    def prepare_bridge(self,navigate=True):
        if not self.session.suspend():
            self.w.status.setText('Идёт запись в контроллер. Дождитесь завершения операции.')
            return False
        if navigate and SECTIONS[self.current][1]:self.nav.setCurrentRow(1)
        return True

    def stop_then_open(self):
        self.w.stop()
        self.retry_open()

    def retry_open(self):
        if self.w.running() or self.w.external_pids():
            QTimer.singleShot(250,self.retry_open);return
        if self.pending is not None and self.nav.currentRow()==self.pending:
            target=self.pending;self.pending=None;self.navigate(target)


def build_ui(w):
    apply_palette()
    w.resize(1280,880);w.setMinimumSize(1060,740)
    w.setStyleSheet(STYLE+EXTRA_STYLE)
    w.setWindowIcon(QIcon(str(BASE/'assets/logo.png')))
    w.console=Diagnostics(w)
    # Constructor navigation calls refresh; the periodic timer starts later.
    w.workspace=Workspace(w)
