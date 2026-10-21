# SPDX-License-Identifier: GPL-3.0-or-later
"""Paths shared by a checkout, per-user install and AppImage."""
import os
from pathlib import Path
import shutil
ROOT=Path(__file__).resolve().parent.parent
ENGINE=os.environ.get('APEXSENSE_ENGINE') or str(
    next((p for p in (ROOT/'build/ApexSenseBridge',Path.home()/'.local/bin/ApexSenseBridge') if p.is_file()),
         Path(shutil.which('ApexSenseBridge') or 'ApexSenseBridge')))
VERSION=(ROOT/'VERSION').read_text().strip() if (ROOT/'VERSION').is_file() else '0.1.0'
