#pragma once

#include <QStringList>

// Headless commands for scripts and agents: clothbound info, list, add, notes,
// and skill. They need no display, print JSON with --json, and exit 0 or 1
// with the reason on stderr.
bool isCliCommand(const QString &word);
QString cliSummary();
int runCli(const QStringList &arguments);
