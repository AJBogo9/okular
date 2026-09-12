/*
    SPDX-FileCopyrightText: 2014 Frederik Gladhorn <gladhorn@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "debug_ui.h"

Q_LOGGING_CATEGORY(OkularUiDebug, "org.kde.okular.ui", QtWarningMsg)

// Scroll pipeline trace: wheel input, scroller state and actual content moves,
// with timestamps, so smoothness can be measured without screen capture.
Q_LOGGING_CATEGORY(OkularScrollTrace, "org.kde.okular.scrolltrace", QtWarningMsg)
