// Lean WebEngine-free stdio MCP server binary for Clothbound.
// Shares Clothbound's read-only library/index/settings paths without linking
// QtWebEngine, QtQuick, QtQml, or QtMultimedia.

#include "app/cli.h"
#include "mcp/mcpserver.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>

#ifndef CLOTHBOUND_VERSION
#define CLOTHBOUND_VERSION "0.4.1"
#endif

// Satisfies McpServer's static CLI registrar without pulling in the full CLI.
void registerMcpRunner(McpRunner) {}

namespace {

void printUsage() {
    std::fputs(
        "Usage: clothbound-mcp [mcp] [--version | --help]\n"
        "\n"
        "Run Clothbound's strictly read-only local stdio Model Context Protocol (MCP)\n"
        "server for coding and research agents.\n"
        "\n"
        "Options:\n"
        "  -h, --help     Display this help and exit.\n"
        "  -v, --version  Display version information and exit.\n",
        stdout);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char *argv[]) {
    McpServer::installStderrMessageHandler();

    QCoreApplication app(argc, argv);
    // Keep the exact application name "clothbound" so QStandardPaths resolves
    // the user's Clothbound library.sqlite, index.sqlite, and settings.ini.
    app.setApplicationName(QStringLiteral("clothbound"));
    app.setOrganizationDomain(QStringLiteral("clothbound.app"));
    app.setApplicationVersion(QStringLiteral(CLOTHBOUND_VERSION));

    const QStringList args = app.arguments();
    for (qsizetype i = 1; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QLatin1String("--version") || arg == QLatin1String("-v")) {
            std::printf("clothbound-mcp %s\n", CLOTHBOUND_VERSION);
            std::fflush(stdout);
            return 0;
        }
        if (arg == QLatin1String("--help") || arg == QLatin1String("-h") ||
            (i == 1 && arg == QLatin1String("help"))) {
            printUsage();
            return 0;
        }
    }

    if (args.size() == 1 || (args.size() == 2 && args.at(1) == QLatin1String("mcp")))
        return McpServer::runStdio(args);

    std::fprintf(stderr, "clothbound-mcp: unexpected argument '%s'. Use --help for usage.\n",
                 qPrintable(args.at(1)));
    std::fflush(stderr);
    return 1;
}
