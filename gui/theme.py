# SPDX-License-Identifier: GPL-3.0-or-later
"""Qt adaptation of the upstream ApexSenseBridge console theme; see NOTICE."""
import json
from pathlib import Path
from PySide6.QtCore import Qt, QProcess
from PySide6.QtGui import QPixmap, QIcon
from PySide6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QLabel, QPushButton,
 QFrame, QScrollArea, QDialog, QTabWidget, QComboBox, QCheckBox, QPlainTextEdit,
 QFormLayout, QLineEdit, QTableWidget, QTableWidgetItem, QHeaderView, QSlider,
 QSpinBox, QMessageBox)

BASE = Path(__file__).parent
STYLE = '''
QMainWindow, QDialog { background: #060A16; }
QWidget { color: #F4F6FB; font-family: "Noto Sans"; font-size: 13px; }
QWidget#shell { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #0E1D45,stop:.5 #081128,stop:1 #04060E); }
QFrame#card { background: #111A31; border: 1px solid #23304A; border-radius: 10px; }
QLabel { background: transparent; border: none; }
QLabel#muted { color: #AAB4CC; font-size: 11px; }
QLabel#title { font-size: 17px; font-weight: 600; }
QLabel#badge { background: #142C50; color: #8DB7FF; border-radius: 6px; padding: 5px 10px; font-size: 11px; font-weight: 600; }
QLabel#pill { background: #1B2540; border-radius: 5px; color: #AAB4CC; padding: 4px 8px; font-size: 10px; }
QPushButton { background: #1B2540; border: 1px solid #3A4768; border-radius: 6px; padding: 8px 13px; color: #D9E2F5; }
QPushButton:hover { border-color: #5B8CFF; background: #23365C; }
QPushButton:pressed { background: #124A9E; }
QPushButton:focus, QComboBox:focus, QSpinBox:focus { border: 2px solid #8DB7FF; }
QPushButton#primary { background: #1E6FE8; border-color: #1E6FE8; color: white; font-weight: 600; }
QPushButton#primary:hover { background: #185CC4; }
QPushButton:disabled { color: #6E7995; background: #141C30; border-color: #25314A; }
QCheckBox { spacing: 10px; padding: 4px; }
QCheckBox::indicator { width: 34px; height: 18px; border-radius: 9px; background: #2A3552; border: 1px solid #3A4768; }
QCheckBox::indicator:checked { background: #1E6FE8; border: 2px solid #8DB7FF; }
QCheckBox::indicator:disabled { background: #141C30; border-color: #25314A; }
QLineEdit, QComboBox, QSpinBox { background: #1B2540; border: 1px solid #3A4768; border-radius: 6px; padding: 7px; selection-background-color: #1E6FE8; }
QComboBox QAbstractItemView { background: #1B2540; selection-background-color: #1E6FE8; }
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 8px; }
QScrollBar::handle:vertical { background: #3A4768; min-height: 30px; border-radius: 4px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QTabWidget::pane { border: none; }
QTabBar::tab { color: #AAB4CC; padding: 13px 20px; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: white; border-bottom: 2px solid #5B8CFF; }
QPlainTextEdit, QTableWidget { background: #0B1225; border: 1px solid #23304A; border-radius: 8px; padding: 8px; selection-background-color: #234D8C; }
QHeaderView::section { background: #111A31; color: #AAB4CC; padding: 10px; border: none; }
QSlider::groove:horizontal { height: 5px; background: #2A3552; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #1E6FE8; border-radius: 2px; }
QSlider::handle:horizontal { background: white; width: 15px; margin: -5px 0; border-radius: 7px; }
QToolTip { background: #1B2540; color: white; border: 1px solid #3A4768; }
'''

def label(text, name=None):
    widget = QLabel(text)
    if name: widget.setObjectName(name)
    widget.setWordWrap(True)
    return widget

def card(parent_layout):
    widget = QFrame()
    widget.setObjectName('card')
    layout = QVBoxLayout(widget)
    layout.setContentsMargins(17, 14, 17, 14)
    layout.setSpacing(10)
    parent_layout.addWidget(widget)
    return layout

def art(name, width, height):
    image = QLabel()
    image.setPixmap(QPixmap(str(BASE/'assets'/name)).scaled(width,height,Qt.KeepAspectRatio,Qt.SmoothTransformation))
    image.setAlignment(Qt.AlignCenter)
    return image

def update_badge(w):
    busy = w.running() or w.external
    w.badge.setText('МОСТ АКТИВЕН' if busy else 'ОЖИДАНИЕ')
    w.badge.setStyleSheet('background: #123A35; color: #34D399; border-radius:6px; padding:5px 10px;' if busy else '')
