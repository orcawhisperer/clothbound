#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cstdio>
#include <functional>

// Pure JSON-RPC 2.0 protocol layer for Clothbound's local stdio MCP server:
// bounded NUL-safe framing, response/error builders, tool & resource schemas,
// untrusted-content boundary formatting, and stderr log routing.
namespace McpProtocol {

inline constexpr int kErrParse = -32700;
inline constexpr int kErrInvalidRequest = -32600;
inline constexpr int kErrMethodNotFound = -32601;
inline constexpr int kErrInvalidParams = -32602;
inline constexpr int kErrMcpDisabled = -32001;
inline constexpr int kErrResourceNotFound = -32002;

inline constexpr int kDefaultReadMaxChars = 8000;
inline constexpr int kMinReadMaxChars = 100;
inline constexpr int kHardCapReadMaxChars = 24000;
inline constexpr qsizetype kMaxStdioFrameBytes = 2 * 1024 * 1024;

// Routes all Qt debug/info/warning/critical/fatal logs to stderr so stdout
// carries exclusively newline-delimited JSON-RPC 2.0 frames.
void installStderrMessageHandler();

QJsonObject jsonRpcResult(const QJsonValue &id, const QJsonObject &result);
QJsonObject jsonRpcError(const QJsonValue &id, int code, const QString &message);

// Wraps untrusted book/library text in explicit prompt-injection boundary markers,
// neutralizing any literal closing marker embedded in the text.
QString wrapUntrustedData(const QString &data);

QJsonObject toolSuccess(const QString &text, const QJsonObject &structured);
QJsonObject toolSuccessJson(const QJsonObject &payload);
QJsonObject toolSuccessUntrustedJson(const QJsonObject &payload);
QJsonObject toolError(const QString &message);
QJsonObject readOnlyAnnotations();

// Strict integer parameter reader (rejects non-numbers, NaN/Inf, fractions, overflow).
bool readOptionalInt(const QJsonObject &obj, const QString &key, int *valueOut, bool *presentOut = nullptr);

// MCP protocol handshake & schema builders.
QString negotiateProtocolVersion(const QString &requested);
QJsonObject buildInitializeResult(const QString &protocolVersion, const QString &serverVersion);
QJsonArray buildToolDefinitions();
QJsonArray buildResourceTemplates();

// Runs a bounded, NUL-safe newline-delimited JSON-RPC 2.0 stdio loop.
void runStdioLoop(std::FILE *in,
                  std::FILE *out,
                  const std::function<QJsonObject(const QJsonObject &)> &handler,
                  qsizetype maxFrameBytes = kMaxStdioFrameBytes);

}  // namespace McpProtocol
