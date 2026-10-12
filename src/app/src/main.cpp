#include <lcad/app/Application.h>
#include <lcad/app/MainWindow.h>
#include <lcad/viewport/ViewportWidget.h>

#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char* argv[]) {
    lcad::viewport::prepareApplication();

    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("LCAD"));
    QApplication::setApplicationVersion(lcad::app::versionString());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Local-first parametric 3D CAD."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(application);

    lcad::app::MainWindow window;
    window.show();
    return QApplication::exec();
}
