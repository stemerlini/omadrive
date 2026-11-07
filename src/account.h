#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTcpServer>
#include <QTimer>

// Microsoft sign-in for the OneDrive pane: opens the browser, catches the
// redirect on a loopback port, and hands the code to Graph.
class Account : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)  // signedOut, signingIn, signedIn
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY stateChanged)
    Q_PROPERTY(QString name READ name NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(bool restoring READ restoring NOTIFY stateChanged)  // reconnecting with the saved sign-in

public:
    explicit Account(QObject *parent = nullptr);

    QString state() const { return m_state; }
    bool signedIn() const { return m_state == "signedIn"; }
    QString name() const { return m_name; }
    QString error() const { return m_error; }
    bool restoring() const { return m_restoring; }

    Q_INVOKABLE void signIn();
    Q_INVOKABLE void signOut();
    Q_INVOKABLE void restore();

signals:
    void stateChanged();

private:
    struct Outcome {
        bool ok = false;
        QString error, name;
    };

    void setState(const QString &state, const QString &error = {});
    void handleConnection();
    void stopServer();
    void finish(const QString &code);
    void watch(std::function<Outcome()> job);

    bool m_restoring = false;
    QString m_state = "signedOut", m_name, m_error;
    QPointer<QTcpServer> m_server;
    QTimer m_timeout;
    QString m_verifier, m_expectedState, m_redirect;
};
