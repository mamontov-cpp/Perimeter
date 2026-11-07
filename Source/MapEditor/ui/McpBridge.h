// McpBridge — the MCP layer as its own class (docs/architecture.md §5.37).
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

namespace MapEditor {
class BrushTool;
class BrushUiController;
class BattleMapController;
class EditorSession;
class McpServer;
class ViewportWidget;

/// The embedded MCP server bridge: owns the QHttpServer wrapper and the
/// tool dispatch (invoke -> per-domain handlers). Absorbed from MainWindow
/// (god-object decomposition, docs/architecture.md §5.37); the handlers call
/// the same core paths as the UI actions, through the borrowed session /
/// viewport / brush tool / battle-map / brush-UI pointers. The four host
/// services are synchronous std::function hooks (world unload/load, status
/// bar refresh, palette repaint).
class McpBridge : public QObject
{
    Q_OBJECT

public:
    McpBridge(MapEditor::EditorSession *session,
              MapEditor::ViewportWidget *viewport,
              MapEditor::BrushTool *brushTool,
              MapEditor::BattleMapController *battleMaps,
              MapEditor::BrushUiController *brushUI, QObject *parent = nullptr);
    ~McpBridge() override;

    /// Starts the server on the configured port (map_editor.ini [mcp] port=,
    /// default 10102). False when the port is busy — the editor keeps
    /// running without the server.
    bool start();

    void setWorldUnloadHook(std::function<void()> hook);
    void setWorldLoadedHook(std::function<void()> hook);
    void setStatusBarHook(std::function<void()> hook);
    void setPaletteRefreshHook(std::function<void()> hook);

    /// Dispatch: tries the domain handlers in order until one claims the
    /// tool (each returns {"__unhandled": true} for foreign names).
    QVariantMap invoke(const QString& name, const QVariantMap& args);

private:
    // Domain handlers — one per tool group.
    QVariantMap mcpHandleSession(const QString& name, const QVariantMap& args);
    QVariantMap mcpHandleBattleMapFiles(const QString& name,
                                        const QVariantMap& args);
    QVariantMap mcpHandlePlayers(const QString& name, const QVariantMap& args);
    QVariantMap mcpHandleObjects(const QString& name, const QVariantMap& args);
    QVariantMap mcpHandleBrushes(const QString& name, const QVariantMap& args);
    /// The apply_brush body (kept out of the handler so the tool-group
    /// handler stays readable).
    QVariantMap mcpApplyBrush(const QVariantMap& args);
    QVariantMap mcpHandleDifference(const QString& name,
                                    const QVariantMap& args);
    QVariantMap mcpHandleTerrain(const QString& name, const QVariantMap& args);
    QVariantMap mcpHandleHistory(const QString& name, const QVariantMap& args);
    /// Shared argument plumbing for the handlers ("" = requirement met).
    QString mcpRequireGameDir() const;
    QString mcpRequireWorld() const;
    QString mcpRequireBattleMap() const;
    static QVariantMap mcpUnhandled();
    static bool mcpIsUnhandled(const QVariantMap& result);
    static int mcpArgInt(const QVariantMap& args, const char* key,
                         int fallback = 0);
    static double mcpArgDbl(const QVariantMap& args, const char* key,
                            double fallback = 0.0);
    static QString mcpArgStr(const QVariantMap& args, const char* key);
    /// Applies the shared player properties (name/faction/color/clan/
    /// difficulty/handicap) plus the terrain-corrected anchor move to the
    /// player at `index` — the common tail of add_player and update_player.
    QVariantMap mcpApplyPlayerArgs(int index, const QVariantMap& args);

    MapEditor::EditorSession *session_ = nullptr;
    MapEditor::ViewportWidget *viewport_ = nullptr;
    MapEditor::BrushTool *brushTool_ = nullptr;
    MapEditor::BattleMapController *battleMaps_ = nullptr;
    MapEditor::BrushUiController *brushUI_ = nullptr;
    MapEditor::McpServer* mcpServer_ = nullptr;
    std::function<void()> worldUnloadHook_;
    std::function<void()> worldLoadedHook_;
    std::function<void()> statusBarHook_;
    std::function<void()> paletteRefreshHook_;
};

} // namespace MapEditor
