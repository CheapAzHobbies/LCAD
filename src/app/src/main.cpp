// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/Application.h>
#include <lcad/app/MainWindow.h>
#include <lcad/viewport/ViewportWidget.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QMessageBox>

int main(int argc, char* argv[]) {
    lcad::viewport::prepareApplication();

    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("LCAD"));
    QApplication::setOrganizationName(QStringLiteral("LCAD"));
    QApplication::setApplicationVersion(lcad::app::versionString());
    QApplication::setDesktopFileName(QStringLiteral("io.github.cheapazhobbies.LCAD"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/io.github.cheapazhobbies.LCAD.png")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Local-first parametric 3D CAD."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("file"),
                                 QStringLiteral("An LCAD document (.lcad) to open."),
                                 QStringLiteral("[file]"));
    parser.process(application);

    lcad::app::startLogging();

    lcad::app::MainWindow window;
    window.show();
    const QStringList files = parser.positionalArguments();
    if (!files.isEmpty()) {
        QString error;
        if (!window.loadFrom(files.first(), &error)) {
            QMessageBox::critical(&window, QStringLiteral("Could not open the document"), error);
        } else if (!window.loadWarnings().isEmpty()) {
            QMessageBox::warning(&window, QStringLiteral("The document was opened with problems"),
                                 window.loadWarnings().join(QLatin1Char('\n')));
        }
    }
    const int result = QApplication::exec();
    lcad::app::stopLogging();
    return result;
}
