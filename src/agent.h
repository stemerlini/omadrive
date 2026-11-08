#pragma once

#include <QJsonObject>
#include <QStringList>

// omadrive agent <command> [args]: a headless, JSON line protocol so agents
// (via the MCP server) can list, search, read, download, upload, rename,
// create, share and trash OneDrive items. Honours the same account safety as
// the app: every command names its account, and work files cannot be copied
// into another account. Prints one JSON object to stdout; exit 0 on success.
int runAgent(const QStringList &args);

// The accounts.json entry for an id or email (the active account when empty),
// with Mirror's account folders loaded; empty with an error when none matches.
QJsonObject resolveAccount(const QString &wanted, QString *error);
QStringList accountIds();
