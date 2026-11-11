#pragma once

#include <QStringList>

// pathfinder agent <command> [args]: a headless JSON line protocol so agents
// (via the MCP server) can list, search, read, download, upload, rename,
// create, share and trash OneDrive items through rclone. Work files
// never leave their account. Prints one JSON object to stdout; exit 0 on
// success.
int runAgent(const QStringList &args);
