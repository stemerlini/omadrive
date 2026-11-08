// omadrive — a two-pane file manager for local folders and OneDrive.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml>
#include <QQuickStyle>
#include <QUrl>

#include "account.h"
#include "backend.h"
#include "folderlist.h"
#include "mirror.h"
#include "onedrivelist.h"
#include "preview.h"
#include "theme.h"
#include "transfer.h"
#include "translator.h"

int main(int argc, char *argv[]) {
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
    Account account;
    OneDriveList::setAccount(&account);
    // Tabs create their own lists from QML.
    qmlRegisterType<FolderList>("Omadrive", 1, 0, "FolderList");
    qmlRegisterType<OneDriveList>("Omadrive", 1, 0, "OneDriveList");
    Transfer transfer;
    Mirror mirror;
    Preview preview;
    account.restore();

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("account", &account);
    engine.rootContext()->setContextProperty("transfer", &transfer);
    engine.rootContext()->setContextProperty("mirror", &mirror);
    engine.rootContext()->setContextProperty("preview", &preview);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
