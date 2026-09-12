/*
    SPDX-FileCopyrightText: 2014 Frederik Gladhorn <gladhorn@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef OKULAR_DEBUG_PDF_H
#define OKULAR_DEBUG_PDF_H

#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(OkularPdfDebug)

/**
 * One line per rasterisation, carrying the render slot and whether the render
 * completed or was abandoned. Off by default; enable with
 * QT_LOGGING_RULES="org.kde.okular.rendertrace.debug=true".
 */
Q_DECLARE_LOGGING_CATEGORY(OkularRenderTrace)

#endif
