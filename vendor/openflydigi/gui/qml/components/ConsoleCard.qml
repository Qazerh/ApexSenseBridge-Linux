// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.kirigamiaddons.formcard as FormCard
FormCard.FormCard {
    // Keep a shared gutter even on compact windows, so corners stay rounded.
    maximumWidth: Math.max(0, Math.min(880, width - 32))
}
