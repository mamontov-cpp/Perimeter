// ============================================================================
// Embedded MCP server — protocol, catalog, lifecycle.
//
// Drives the real McpServer (QHttpServer + JSON-RPC 2.0 over HTTP POST /mcp)
// over the loopback interface with a QNetworkAccessManager client, exactly
// like an LLM agent would:
//   * initialize / tools/list / tools/call / notifications / errors;
//   * catalog sanity: every tool has a name, an agent-readable description
//     and an inputSchema; all spec-required tools are present;
//   * the 500 ms undo-grouping note appears in the undo tool and the
//     mutating tool descriptions;
//   * restart on the SAME port after stop() (no "address already in use"),
//     and a graceful failure when the port is taken;
//   * port configuration from an ini file ([mcp] port=N).
//
// Runs after the nature tests (order 110). Needs the Qt event loop the
// runner's QCoreApplication provides.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#ifdef MAP_EDITOR_MCP

#include "McpServer.h"
#include "McpTools.h"

#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace {

struct HttpResult
{
    int status = 0;
    QJsonObject body;
};

/// POST one JSON-RPC message and wait for the response.
HttpResult postJson(quint16 port, const QJsonObject& payload)
{
    QNetworkAccessManager nam;
    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/mcp").arg(port)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply =
        nam.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));

    HttpResult result;
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit); // safety net
    loop.exec();

    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray raw = reply->readAll();
    result.body = QJsonDocument::fromJson(raw).object();
    if (qEnvironmentVariableIsSet("MCP_TEST_DEBUG")) {
        fprintf(stderr, "HTTP %d: %s\n", result.status, raw.constData());
        fflush(stderr);
    }
reply->deleteLater();
    return result;
}

QJsonObject rpcCall(const QString& method, const QJsonObject& params = QJsonObject(),
                    int id = 1)
{
    QJsonObject msg{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("method"), method},
                    {QStringLiteral("id"), id}};
    if (!params.isEmpty()) {
        msg.insert(QStringLiteral("params"), params);
    }
    return msg;
}

bool runMcpServerTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    // ------------------------------------------------------------------
    // 1: catalog sanity.
    // ------------------------------------------------------------------
    const QVector<MapEditor::McpToolSpec> catalog = MapEditor::mcpToolCatalog();
    TEST_REQUIRE(catalog.size() >= 36,
                 "the catalog must expose at least 25 tools, got %lld.",
                 static_cast<long long>(catalog.size()));
    const QStringList required = {
        QStringLiteral("set_game_path"),   QStringLiteral("list_worlds"),
        QStringLiteral("get_world_properties"), QStringLiteral("create_world"),
        QStringLiteral("open_world"),      QStringLiteral("save_world"),
        QStringLiteral("list_battle_maps"), QStringLiteral("open_battle_map"),
        QStringLiteral("save_battle_map"), QStringLiteral("list_battle_map_objects"),
        QStringLiteral("place_building"),  QStringLiteral("update_building"),
        QStringLiteral("delete_building"), QStringLiteral("place_scum_spot"),
        QStringLiteral("update_scum_spot"), QStringLiteral("delete_scum_spot"),
        QStringLiteral("place_nature_object"), QStringLiteral("update_nature_object"),
        QStringLiteral("delete_nature_object"), QStringLiteral("apply_brush"),
        QStringLiteral("run_script"),      QStringLiteral("set_scripted_brush_script"),
        QStringLiteral("get_terrain_at"),  QStringLiteral("get_palette"),
        QStringLiteral("load_decal_image"), QStringLiteral("import_model"),
        QStringLiteral("set_difference_mode"), QStringLiteral("save_difference"),
        QStringLiteral("load_difference"), QStringLiteral("list_players"),
        QStringLiteral("update_player"), QStringLiteral("remove_player"),
        QStringLiteral("add_player"),
        QStringLiteral("set_palette_color"), QStringLiteral("undo"),
        QStringLiteral("redo"),
    };
    QStringList names;
    for (const MapEditor::McpToolSpec& spec : catalog) {
        TEST_REQUIRE(!spec.name.isEmpty(), "every tool needs a name.");
        TEST_REQUIRE(!spec.description.isEmpty(),
                     "tool %s needs a description.", spec.name.toUtf8().constData());
        TEST_REQUIRE(spec.description.size() > 40,
                     "tool %s description must be informative.",
                     spec.name.toUtf8().constData());
        TEST_REQUIRE(spec.inputSchema.value(QStringLiteral("type")).toString() ==
                         QStringLiteral("object"),
                     "tool %s schema must be a JSON Schema object.",
                     spec.name.toUtf8().constData());
        names << spec.name;
    }
    for (const QString& want : required) {
        TEST_REQUIRE(names.contains(want), "required tool missing: %s.",
                     want.toUtf8().constData());
    }
    // The 500 ms undo-grouping note must reach the agent.
    const auto undoTool = std::find_if(
        catalog.begin(), catalog.end(), [](const MapEditor::McpToolSpec& s) {
            return s.name == QStringLiteral("undo");
        });
    TEST_REQUIRE(undoTool != catalog.end() &&
                     undoTool->description.contains(QStringLiteral("500 ms")),
                 "the undo description must mention the 500 ms grouping.");
    const auto brushTool = std::find_if(
        catalog.begin(), catalog.end(), [](const MapEditor::McpToolSpec& s) {
            return s.name == QStringLiteral("apply_brush");
        });
    TEST_REQUIRE(brushTool != catalog.end() &&
                     brushTool->description.contains(QStringLiteral("500 ms")),
                 "the apply_brush description must mention the 500 ms grouping.");
    std::printf("[PASS] MCP catalog: %lld tools, descriptions, schemas, 500 ms note.\n",
                static_cast<long long>(catalog.size()));

    // ------------------------------------------------------------------
    // 2: ini port configuration.
    // ------------------------------------------------------------------
    const QString iniPath =
        QString::fromStdString((ctx.tempRoot / "mcp_port.ini").u8string());
    {
        QFile f(iniPath);
        f.open(QIODevice::WriteOnly);
        f.write("[mcp]\nport=23456\n");
    }
    TEST_REQUIRE(MapEditor::McpServer::configuredPort(iniPath, 10102) == 23456,
                 "configuredPort must read [mcp] port.");
    TEST_REQUIRE(MapEditor::McpServer::configuredPort(
                     QStringLiteral("/nonexistent/map_editor.ini"), 10102) == 10102,
                 "a missing ini falls back to the default port.");
    {
        QFile f(iniPath);
        f.open(QIODevice::WriteOnly);
        f.write("[mcp]\nport=notanumber\n");
    }
    TEST_REQUIRE(MapEditor::McpServer::configuredPort(iniPath, 10102) == 10102,
                 "an invalid port falls back to the default.");
    std::printf("[PASS] ini port configuration.\n");

    // ------------------------------------------------------------------
    // 3: protocol over HTTP with a stub tool handler.
    // ------------------------------------------------------------------
    MapEditor::McpServer server;
    server.setTools(catalog);
    server.setToolHandler([](const QString& name, const QVariantMap& args) {
        if (name == QLatin1String("redo")) {
            return QVariantMap{{QStringLiteral("error"), QStringLiteral("boom")}};
        }
        QVariantMap out{{QStringLiteral("echo"), name}};
        for (auto it = args.begin(); it != args.end(); ++it) {
            out.insert(QStringLiteral("arg_") + it.key(), it.value());
        }
        return out;
    });
    QString startError;
    TEST_REQUIRE(server.start(0, &startError), "start failed: %s",
                 startError.toUtf8().constData());
    const quint16 port = server.port();
    TEST_REQUIRE(port > 0, "an ephemeral port must be assigned.");

    // initialize.
    {
        const HttpResult r =
            postJson(port, rpcCall(QStringLiteral("initialize"),
                                   QJsonObject{
                                       {QStringLiteral("protocolVersion"),
                                        QStringLiteral("2025-06-18")}}));
        TEST_REQUIRE(r.status == 200, "initialize must return 200, got %d.",
                     r.status);
        TEST_REQUIRE(r.body.value(QStringLiteral("result"))
                         .toObject()
                         .value(QStringLiteral("serverInfo"))
                         .toObject()
                         .value(QStringLiteral("name"))
                         .toString() == QStringLiteral("perimeter-map-editor"),
                     "initialize must identify the server.");
    }

    // tools/list matches the catalog.
    {
        const HttpResult r = postJson(port, rpcCall(QStringLiteral("tools/list")));
        const QJsonArray tools =
            r.body.value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
        TEST_REQUIRE(tools.size() == catalog.size(),
                     "tools/list must expose the full catalog (%lld vs %lld).",
                     static_cast<long long>(tools.size()),
                     static_cast<long long>(catalog.size()));
    }

    // tools/call: success payload, argument pass-through, error mapping.
    {
        const HttpResult r = postJson(
            port, rpcCall(QStringLiteral("tools/call"),
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("undo")},
                                      {QStringLiteral("arguments"),
                                       QJsonObject{{QStringLiteral("x"), 7}}}}));
        const QJsonObject result = r.body.value(QStringLiteral("result")).toObject();
        TEST_REQUIRE(!result.value(QStringLiteral("isError")).toBool(),
                     "a successful call must not be an error.");
        const QJsonObject text =
            QJsonDocument::fromJson(result.value(QStringLiteral("content"))
                                        .toArray()
                                        .first()
                                        .toObject()
                                        .value(QStringLiteral("text"))
                                        .toString()
                                        .toUtf8())
                .object();
        TEST_REQUIRE(text.value(QStringLiteral("echo")).toString() ==
                         QStringLiteral("undo"),
                     "the result must carry the tool output.");
        TEST_REQUIRE(text.value(QStringLiteral("arg_x")).toInt() == 7,
                     "arguments must reach the handler.");
    }
    {
        const HttpResult r = postJson(
            port, rpcCall(QStringLiteral("tools/call"),
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("redo")},
                                      {QStringLiteral("arguments"), QJsonObject()}}));
        const QJsonObject result = r.body.value(QStringLiteral("result")).toObject();
        TEST_REQUIRE(result.value(QStringLiteral("isError")).toBool() == true,
                     "an {error} handler result must map to isError.");
    }
    {
        // Unknown tool -> JSON-RPC error.
        const HttpResult r = postJson(
            port, rpcCall(QStringLiteral("tools/call"),
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("nope")},
                                      {QStringLiteral("arguments"), QJsonObject()}}));
        TEST_REQUIRE(r.body.contains(QStringLiteral("error")),
                     "an unknown tool must produce a JSON-RPC error.");
    }

    // Notification (no id) -> 202.
    {
        QJsonObject note{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("method"), QStringLiteral("notifications/initialized")}};
        const HttpResult r = postJson(port, note);
        TEST_REQUIRE(r.status == 202, "a notification must get 202, got %d.",
                     r.status);
    }

    // Unknown method -> -32601.
    {
        const HttpResult r =
            postJson(port, rpcCall(QStringLiteral("resources/read")));
        TEST_REQUIRE(r.body.value(QStringLiteral("error"))
                         .toObject()
                         .value(QStringLiteral("code"))
                         .toInt() == -32601,
                     "unknown methods must fail with -32601.");
    }

    // GET info page.
    {
        QNetworkAccessManager nam;
        QNetworkReply* reply = nam.get(QNetworkRequest(
            QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(port))));
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
        TEST_REQUIRE(QJsonDocument::fromJson(reply->readAll())
                         .object()
                         .contains(QStringLiteral("name")),
                     "GET / must serve the server info.");
        reply->deleteLater();
    }
    std::printf("[PASS] MCP protocol: initialize/list/call/notify/errors/GET.\n");

    // ------------------------------------------------------------------
    // 4: restart on the same port + busy-port failure.
    // ------------------------------------------------------------------
    {
        const quint16 fixedPort = server.port();
        server.stop();
        TEST_REQUIRE(!server.isRunning(), "stop() must shut the server down.");
        QString restartError;
        TEST_REQUIRE(server.start(fixedPort, &restartError),
                     "restart on the same port failed: %s",
                     restartError.toUtf8().constData());
        TEST_REQUIRE(server.port() == fixedPort, "the same port must rebind.");
        const HttpResult r =
            postJson(fixedPort, rpcCall(QStringLiteral("ping")));
        TEST_REQUIRE(r.body.value(QStringLiteral("result")).isObject(),
                     "the restarted server must answer.");

        // A second server on the occupied port must fail gracefully.
        MapEditor::McpServer other;
        QString busyError;
        TEST_REQUIRE(!other.start(fixedPort, &busyError),
                     "binding a busy port must fail.");
        TEST_REQUIRE(!busyError.isEmpty(), "the failure must explain itself.");
        server.stop();
    }
    std::printf("[PASS] MCP lifecycle: restart rebinds, busy port fails cleanly.\n");

    return true;
}

MAP_EDITOR_REGISTER_TEST("MCP server", 110, runMcpServerTests);

} // namespace

#endif // MAP_EDITOR_MCP
