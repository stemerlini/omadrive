#pragma once

#include <QStringList>

// omadrive mount [--account <id|email>] [--foreground] [--mountpoint <dir>]:
// shows the account's whole OneDrive in its folder (~/OneDrive-<Name>) as a
// FUSE file system. Files download when opened and changes upload a few
// seconds after the last write; see SyncCore.
int runMount(const QStringList &args);
// omadrive unmount [--account <id|email>] [--mountpoint <dir>]
int runUnmount(const QStringList &args);
// The folder an account is mounted at, from its status file; empty when it
// is not mounted.
QString mountedAt(const QString &account);
