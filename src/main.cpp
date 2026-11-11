// omadrive — a two-pane file manager; folders in rclone OneDrive mounts show
// their cloud state.

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml>
#include <QQuickStyle>
#include <QUrl>

#include "agent.h"
#include "backend.h"
#include "cloud.h"
#include "folderlist.h"
#include "preview.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    if (argc >= 2 && QByteArray(argv[1]) == "cloud") {
        QCoreApplication app(argc, argv);
        app.setApplicationName("omadrive");
        QStringList args;
        for (int i = 2; i < argc; ++i)
            args << QString::fromLocal8Bit(argv[i]);
        return runCloud(args);
    }
    if (argc >= 2 && QByteArray(argv[1]) == "agent") {
        QCoreApplication app(argc, argv);
        app.setApplicationName("omadrive");
        QStringList args;
        for (int i = 2; i < argc; ++i)
            args << QString::fromLocal8Bit(argv[i]);
        return runAgent(args);
    }
    QGuiApplication app(argc, argv);
    app.setApplicationName("omadrive");
    app.setApplicationVersion("0.1.0");

    // Matches the window to omadrive.desktop, so the compositor (Wayland
    // app_id) and the launcher pick up the installed icon.
    app.setDesktopFileName("omadrive");
    app.setWindowIcon(QIcon::fromTheme("omadrive"));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;
    // Tabs create their own lists from QML.
    qmlRegisterType<FolderList>("Omadrive", 1, 0, "FolderList");
    Preview preview;

    // `omadrive <folder>` opens that folder in a new tab.
    QString startFolder;
    for (const QString &arg : app.arguments().mid(1)) {
        if (!arg.startsWith('-') && QFileInfo(arg).isDir()) {
            startFolder = QDir::cleanPath(QFileInfo(arg).absoluteFilePath());
            break;
        }
    }

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("preview", &preview);
    engine.rootContext()->setContextProperty("freshStart", app.arguments().contains("--new-window"));
    engine.rootContext()->setContextProperty("startFolder", startFolder);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
