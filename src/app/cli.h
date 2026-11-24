#pragma once

#include <QStringList>

// Headless commands for scripts and agents: clothbound info, list, add, notes,
// search, passage, mcp, voices, ai, and skill. They need no display, print
// JSON with --json, and exit 0 or 1 with the reason on stderr.
using McpRunner = int (*)(const QStringList &arguments);
void registerMcpRunner(McpRunner runner);
bool isCliCommand(const QString &word);
QString cliSummary();
int runCli(const QStringList &arguments);
