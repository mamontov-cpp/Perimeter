// ============================================================================
// MapEditorMcp — McpServer implementation (see the header).
// ============================================================================

#include "McpServer.h"

#include <QFileInfo>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QSettings>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace MapEditor {

namespace {

const char kProtocolVersion[] = "2025-06-18";

QHttpServerResponse jsonResponse(const QJsonObject& body, int status = 200)
{
    return QHttpServerResponse(body, QHttpServerResponse::StatusCode(status));
}

QHttpServerResponse jsonRpcResult(const QJsonValue& id, const QJsonValue& result)
{
    return jsonResponse(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("result"), result},
    });
}

QHttpServerResponse jsonRpcError(const QJsonValue& id, int code,
                                 const QString& message)
{
    return jsonResponse(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("error"),
         QJsonObject{{QStringLiteral("code"), code},
                     {QStringLiteral("message"), message}}},
    });
}

} // namespace

McpServer::McpServer(QObject* parent)
    : QObject(parent)
{
}

McpServer::~McpServer()
{
    stop();
}

void McpServer::setTools(const QVector<McpToolSpec>& tools)
{
    tools_ = tools;
}

void McpServer::setToolHandler(ToolHandler handler)
{
    handler_ = std::move(handler);
}

QHttpServerResponse McpServer::handleRpc(const QHttpServerRequest& request)
{
    const QJsonDocument doc =
        QJsonDocument::fromJson(request.body());
    if (!doc.isObject()) {
        return jsonRpcError(QJsonValue::Null, -32700,
                            QStringLiteral("parse error"));
    }
    const QJsonObject call = doc.object();
    const QJsonValue id = call.value(QStringLiteral("id"));
    const bool isNotification = id.isUndefined() || id.isNull();
    const QString method = call.value(QStringLiteral("method")).toString();

    auto reply = [&](QHttpServerResponse&& response) {
        // MCP notifications get a 202 with an empty body.
        if (isNotification) {
            return QHttpServerResponse(QHttpServerResponse::StatusCode::Accepted);
        }
        return std::move(response);
    };

    if (method == QStringLiteral("initialize")) {
        return reply(jsonRpcResult(
            id,
            QJsonObject{
                {QStringLiteral("protocolVersion"),
                 call.value(QStringLiteral("params"))
                     .toObject()
                     .value(QStringLiteral("protocolVersion"))
                     .toString(QLatin1String(kProtocolVersion))},
                {QStringLiteral("capabilities"),
                 QJsonObject{{QStringLiteral("tools"), QJsonObject()}}},
                {QStringLiteral("serverInfo"),
                 QJsonObject{{QStringLiteral("name"),
                              QStringLiteral("perimeter-map-editor")},
                             {QStringLiteral("version"), QStringLiteral("1.0")}}},
            }));
    }
    if (method == QStringLiteral("ping")) {
        return reply(jsonRpcResult(id, QJsonObject()));
    }
    if (method == QStringLiteral("tools/list")) {
        QJsonArray tools;
        for (const McpToolSpec& spec : tools_) {
            tools.append(QJsonObject{
                {QStringLiteral("name"), spec.name},
                {QStringLiteral("description"), spec.description},
                {QStringLiteral("inputSchema"), spec.inputSchema},
            });
        }
        return reply(jsonRpcResult(id, QJsonObject{
                                            {QStringLiteral("tools"), tools},
                                        }));
    }
    if (method == QStringLiteral("tools/call")) {
        const QJsonObject params =
            call.value(QStringLiteral("params")).toObject();
        const QString name = params.value(QStringLiteral("name")).toString();
        const QVariantMap args =
            params.value(QStringLiteral("arguments")).toObject().toVariantMap();
        const auto specIt =
            std::find_if(tools_.begin(), tools_.end(), [&](const McpToolSpec& s) {
                return s.name == name;
            });
        if (specIt == tools_.end()) {
            return reply(jsonRpcError(id, -32602,
                                      QStringLiteral("unknown tool: %1").arg(name)));
        }
        if (!handler_) {
            return reply(jsonRpcError(id, -32603,
                                      QStringLiteral("no tool handler registered")));
        }
        const QVariantMap result = handler_(name, args);
        const QString error = result.value(QStringLiteral("error")).toString();
        QJsonObject toolResult;
        if (!error.isEmpty()) {
            toolResult.insert(QStringLiteral("isError"), true);
        }
        QJsonObject payload = QJsonObject::fromVariantMap(result);
        payload.remove(QStringLiteral("error"));
        toolResult.insert(
            QStringLiteral("content"),
            QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"),
                 QString::fromUtf8(
                     QJsonDocument(payload).toJson(QJsonDocument::Compact))},
            }});
        return reply(jsonRpcResult(id, toolResult));
    }
    return reply(jsonRpcError(id, -32601,
                              QStringLiteral("method not found: %1").arg(method)));
}

quint16 McpServer::configuredPort(const QString& iniFilePath,
                                  quint16 defaultPort)
{
    if (iniFilePath.isEmpty() || !QFileInfo::exists(iniFilePath)) {
        return defaultPort;
    }
    QSettings ini(iniFilePath, QSettings::IniFormat);
    const QVariant value = ini.value(QStringLiteral("mcp/port"));
    bool ok = false;
    const int port = value.toInt(&ok);
    if (!ok || port < 0 || port > 65535) {
        return defaultPort;
    }
    return static_cast<quint16>(port == 0 ? defaultPort : port);
}

quint16 McpServer::port() const
{
    return tcpServer_ ? tcpServer_->serverPort() : 0;
}

void McpServer::stop()
{
    // A fresh QHttpServer/QTcpServer pair is built per start(): close the
    // socket and destroy both so the port is released before the next
    // start() (no "address already in use" on restart).
    if (tcpServer_) {
        tcpServer_->close();
        delete tcpServer_;
        tcpServer_ = nullptr;
    }
    if (server_) {
        delete server_;
        server_ = nullptr;
    }
}

bool McpServer::start(quint16 port, QString* errorMessage)
{
    stop();
    auto* server = new QHttpServer(this);
    server->route(QStringLiteral("/mcp"), QHttpServerRequest::Method::Post,
                  [this](const QHttpServerRequest& request) {
                      return handleRpc(request);
                  });
    server->route(QStringLiteral("/"), QHttpServerRequest::Method::Post,
                  [this](const QHttpServerRequest& request) {
                      return handleRpc(request);
                  });
    server->route(QStringLiteral("/"), QHttpServerRequest::Method::Get,
                  [](const QHttpServerRequest&) {
                      return jsonResponse(QJsonObject{
                          {QStringLiteral("name"),
                           QStringLiteral("perimeter-map-editor-mcp")},
                          {QStringLiteral("protocol"), kProtocolVersion},
                          {QStringLiteral("endpoint"),
                           QStringLiteral("/mcp (POST, JSON-RPC 2.0)")},
                      });
                  });
    auto* tcp = new QTcpServer(this);
    if (!tcp->listen(QHostAddress::Any, port)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cannot bind port %1 (%2)")
                                .arg(port)
                                .arg(tcp->errorString());
        }
        delete tcp;
        delete server;
        return false;
    }
    server->bind(tcp);
    server_ = server;
    tcpServer_ = tcp;
    return true;
}

} // namespace MapEditor
