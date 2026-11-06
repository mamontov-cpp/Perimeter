#pragma once

// ============================================================================
// MapEditorMcp — McpServer.
//
// Embedded MCP server for LLM agents, transport = HTTP + JSON-RPC 2.0 via
// QHttpServer (streamable-HTTP-style: stateless POST /mcp). Runs on the
// thread it is created on — the editor instantiates it on the UI thread so
// tool handlers touch Terra/editor state single-threaded, exactly like the
// UI does.
//
// Lifecycle: start(port) / stop() / start(...) again — every start builds a
// fresh QHttpServer, so a re-bind after shutdown never hits "address already
// in use". A failed listen() reports false and leaves no half-open socket.
//
// Port configuration: map_editor.ini next to the executable,
// [mcp] port=10102 (see configuredPort()).
// ============================================================================

#include "McpTools.h"

#include <QHttpServerResponse>
#include <QObject>
#include <QVariantMap>

#include <functional>

class QHttpServer;
class QHttpServerRequest;
class QTcpServer;

namespace MapEditor {

class McpServer : public QObject
{
    // No Q_OBJECT: no signals/slots/properties — moc stays unnecessary for
    // the Qt-free-linkable static lib.

public:
    /// Handles a tools/call: tool name + arguments -> result map. Return a
    /// map with an "error" string key to produce an MCP isError result.
    using ToolHandler =
        std::function<QVariantMap(const QString& name, const QVariantMap& args)>;

    explicit McpServer(QObject* parent = nullptr);
    ~McpServer() override;

    void setTools(const QVector<McpToolSpec>& tools);
    void setToolHandler(ToolHandler handler);

    /// Binds and serves. port 0 picks an ephemeral port (query port()).
    bool start(quint16 port, QString* errorMessage = nullptr);
    void stop();
    bool isRunning() const { return server_ != nullptr; }
    /// The actually bound port (valid while running).
    quint16 port() const;

    /// Reads the bind port from an ini file ([mcp] port=N); falls back to
    /// defaultPort when the file or key is missing or invalid.
    static quint16 configuredPort(const QString& iniFilePath,
                                  quint16 defaultPort = 10102);

private:
    QHttpServerResponse handleRpc(const QHttpServerRequest& request);

    QHttpServer* server_ = nullptr;
    QTcpServer* tcpServer_ = nullptr;
    QVector<McpToolSpec> tools_;
    ToolHandler handler_;
};

} // namespace MapEditor
