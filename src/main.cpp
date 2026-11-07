// omadrive — a two-pane file manager for local folders and OneDrive.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
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
    FolderList left, right;
    left.setPath(backend.homePath());
    right.setPath(backend.homePath());
    Account account;
    OneDriveList oneDrive(&account);
    Transfer transfer;
    Mirror mirror;
    Preview preview;
    account.restore();

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("leftFolder", &left);
    engine.rootContext()->setContextProperty("rightFolder", &right);
    engine.rootContext()->setContextProperty("account", &account);
    engine.rootContext()->setContextProperty("oneDrive", &oneDrive);
    engine.rootContext()->setContextProperty("transfer", &transfer);
    engine.rootContext()->setContextProperty("mirror", &mirror);
    engine.rootContext()->setContextProperty("preview", &preview);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
