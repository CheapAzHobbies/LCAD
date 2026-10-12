// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QDir>
#include <QImage>
#include <QString>

#include <cstdlib>

// Checks on rendered frames. They establish that something was drawn, never that geometry is
// correct; see docs/TESTING.md.
namespace lcad::test {

inline bool isNear(QRgb pixel, const QColor& color, int tolerance = 6) {
    return std::abs(qRed(pixel) - color.red()) <= tolerance &&
           std::abs(qGreen(pixel) - color.green()) <= tolerance &&
           std::abs(qBlue(pixel) - color.blue()) <= tolerance;
}

/// Share of the frame, from 0 to 1, that is not the background color.
inline double drawnFraction(const QImage& frame, const QColor& background) {
    if (frame.isNull()) {
        return 0.0;
    }
    qsizetype drawn = 0;
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            if (!isNear(frame.pixel(x, y), background)) {
                ++drawn;
            }
        }
    }
    return static_cast<double>(drawn) / (static_cast<double>(frame.width()) * frame.height());
}

/// Share of pixels, from 0 to 1, that differ between two images of the same size.
inline double differingFraction(const QImage& a, const QImage& b, int tolerance = 6) {
    if (a.isNull() || a.size() != b.size()) {
        return 1.0;
    }
    qsizetype differing = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (!isNear(a.pixel(x, y), QColor::fromRgb(b.pixel(x, y)), tolerance)) {
                ++differing;
            }
        }
    }
    return static_cast<double>(differing) / (static_cast<double>(a.width()) * a.height());
}

/// Saves `frame` as <name>.png when LCAD_TEST_ARTIFACTS names a directory, so a failing run can
/// be inspected.
inline void keepFrame(const QImage& frame, const char* name) {
    const QString directory = qEnvironmentVariable("LCAD_TEST_ARTIFACTS");
    if (!directory.isEmpty()) {
        QDir().mkpath(directory);
        frame.save(QDir(directory).filePath(QString::fromLatin1(name) + QStringLiteral(".png")));
    }
}

} // namespace lcad::test
