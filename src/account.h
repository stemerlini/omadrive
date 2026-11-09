#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTcpServer>
#include <QTimer>
#include <QVariantList>
#include <functional>

#include "graph.h"

// The Microsoft accounts OneDrive can show, one at a time: say a personal
// one and a work one. Signing in opens the browser and catches the redirect
// on a loopback port. The list lives in ~/.config/omadrive/accounts.json and
// each account's sign-in in the keyring; switching is instant because every
// account stays signed in.
class Account : public QObject {
    Q_OBJECT
    // The active account. state: signedOut, signingIn, signedIn
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY stateChanged)
    Q_PROPERTY(bool restoring READ restoring NOTIFY stateChanged)  // reconnecting with the saved sign-in
    // A browser sign-in is under way, maybe for an account being added.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString activeId READ activeId NOTIFY stateChanged)
    Q_PROPERTY(QString name READ name NOTIFY accountsChanged)
    Q_PROPERTY(QString email READ email NOTIFY accountsChanged)
    Q_PROPERTY(bool business READ business NOTIFY accountsChanged)
    Q_PROPERTY(QString avatar READ avatar NOTIFY accountsChanged)
    // "OneDrive" or "OneDrive - SES", as Windows names them.
    Q_PROPERTY(QString driveName READ driveName NOTIFY accountsChanged)
    // { id, name, email, org, business, driveName, avatar, active, status }
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)

public:
    explicit Account(QObject *parent = nullptr);

    QString state() const;
    bool signedIn() const { return state() == "signedIn"; }
    bool restoring() const;
    bool busy() const { return m_signingIn; }
    QString activeId() const { return m_activeId; }
    QString name() const;
    QString email() const;
    bool business() const;
    QString avatar() const;
    QString driveName() const;
    QVariantList accounts() const;

    // Signs the active account in again, or adds the first one.
    Q_INVOKABLE void signIn();
    Q_INVOKABLE void addAccount();
    // Signs a new account in with a borrowed app (client id, optional tenant),
    // so a work account can use a first-party app that its tenant allows while
    // the personal account keeps the default registration.
    Q_INVOKABLE void addAccountWithApp(const QString &clientId, const QString &tenant);
    Q_INVOKABLE void switchTo(const QString &id);
    Q_INVOKABLE void switchNext();
    // Forgets the active account on this device and shows the next one.
    Q_INVOKABLE void signOut();
    Q_INVOKABLE void restore();

    // ~/OneDrive-Personal, or ~/OneDrive-<domain> (SES for @ses.com) for a work account.
    static QString defaultMirror(const Profile &profile, const QStringList &taken);
    static QString moveMirror(const QString &from, const QString &to);

signals:
    void stateChanged();
    void accountsChanged();
    void busyChanged();
    void notice(const QString &text, bool isError);

private:
    struct Entry {
        Profile profile;
        QString mirror;
        QString status = "connecting";  // connecting, ready, expired
        int avatarVersion = 0;
        QString clientId, tenant;  // app this account signs in with; empty = default
    };
    struct Outcome {
        bool ok = false;
        QString error;
        Profile profile;
        QByteArray photo;
        bool legacy = false;  // the sign-in from before there were accounts
        bool retry = false;   // failed for now; try again next start
        QString clientId, tenant;  // the app this sign-in used
    };

    Entry *entry(const QString &id);
    const Entry *activeEntry() const;
    void load();
    void save() const;
    void changed();
    void setBusy(bool busy);
    void connectAccount(const QString &id);
    void welcome(const Outcome &outcome);
    void writeAvatar(Entry &e, const QByteArray &photo);
    QString avatarFile(const QString &id) const;
    void interactive(const QString &hint, const QString &clientId = {}, const QString &tenant = {});
    void handleConnection();
    void stopServer();
    void finish(const QString &code);
    void run(std::function<Outcome()> job, std::function<void(const Outcome &)> done);

    QList<Entry> m_entries;
    QString m_activeId;
    bool m_signingIn = false;
    QString m_lastKey;
    QPointer<QTcpServer> m_server;
    QTimer m_timeout;
    QString m_verifier, m_expectedState, m_redirect;
    QString m_signInClientId, m_signInTenant;  // app of the sign-in in progress
};
