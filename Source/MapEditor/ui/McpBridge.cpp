// ============================================================================
// McpBridge — the embedded MCP server bridge (docs/architecture.md §5.37).
//
// Wires the embedded MCP server (mcp/McpServer) to the editor: the catalog
// tools dispatch (mcpInvoke) to per-domain handlers — Session, BattleMapFiles,
// Difference, Players, Objects, Brushes, Terrain, History — each calling the
// same core paths as the UI actions and panels. Dialog flows are replaced by
// plain string/number arguments. The server runs on the UI thread, so
// handlers touch vMap/editor state exactly like the widgets do.
//
// Port configuration: map_editor.ini next to the executable,
//   [mcp]
//   port=10102
// ============================================================================

#ifdef MAP_EDITOR_MCP

#include "McpBridge.h"

#include "BattleMapController.h"
#include "BrushUiController.h"

#include "BattleMapPanel.h"
#include "BuildingCatalog.h"
#include "BrushTool.h"
#include "EditorSession.h"
#include "EditorTrace.h"
#include "McpServer.h"
#include "McpTools.h"
#include "ScumSpotCatalog.h"
#include "Shape3D.h"
#include "ImageDecal.h"
#include "ModelBrush.h"
#include "ScriptedBrush.h"
#include "WorldCreation.h"
#include "spg/SpgFile.h"


#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImageReader>
#include <QJsonObject>
#include <QSet>
#include <QVariantMap>

namespace {

QVariantMap mcpError(const QString& message)
{
    return QVariantMap{{QStringLiteral("error"), message}};
}

QVariantMap mcpOk(const QVariantMap& extra = QVariantMap())
{
    QVariantMap ok{{QStringLiteral("ok"), true}};
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        ok.insert(it.key(), it.value());
    }
    return ok;
}

} // namespace

bool MapEditor::McpBridge::start()
{
    mcpServer_ = new MapEditor::McpServer(this);
    mcpServer_->setTools(MapEditor::mcpToolCatalog());
    mcpServer_->setToolHandler(
        [this](const QString& name, const QVariantMap& args) {
            return invoke(name, args);
        });

    const QString iniPath =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("map_editor.ini"));
    QString error;
    if (!mcpServer_->start(MapEditor::McpServer::configuredPort(iniPath, 10102),
                           &error)) {
        // A busy port must not break the editor — log and continue without
        // the server.
        MapEditor::editorTrace("MCP server disabled: %s (%s)",
                    error.toUtf8().constData(),
                    iniPath.toUtf8().constData());
        return false;
    }
    MapEditor::editorTrace("MCP server listening on port %u (config: %s)",
                mcpServer_->port(), iniPath.toUtf8().constData());
    return true;
}

// ---------------------------------------------------------------------------
// Shared argument plumbing
// ---------------------------------------------------------------------------

QString MapEditor::McpBridge::mcpRequireGameDir() const
{
    if (!session_ || !session_->hasGameDir()) {
        return QStringLiteral("set the game path first (set_game_path)");
    }
    return QString();
}

QString MapEditor::McpBridge::mcpRequireWorld() const
{
    const QString dirError = mcpRequireGameDir();
    if (!dirError.isEmpty()) {
        return dirError;
    }
    if (!session_->isWorldLoaded()) {
        return QStringLiteral("no world loaded (open_world first)");
    }
    return QString();
}

QString MapEditor::McpBridge::mcpRequireBattleMap() const
{
    const QString worldError = mcpRequireWorld();
    if (!worldError.isEmpty()) {
        return worldError;
    }
    if (!battleMaps_->spgFile().loaded()) {
        return QStringLiteral("no battle map loaded (open_battle_map first)");
    }
    return QString();
}

QVariantMap MapEditor::McpBridge::mcpUnhandled()
{
    return QVariantMap{{QStringLiteral("__unhandled"), true}};
}

bool MapEditor::McpBridge::mcpIsUnhandled(const QVariantMap& result)
{
    return result.value(QStringLiteral("__unhandled")).toBool();
}

int MapEditor::McpBridge::mcpArgInt(const QVariantMap& args, const char* key, int fallback)
{
    return args.value(QLatin1String(key), fallback).toInt();
}

double MapEditor::McpBridge::mcpArgDbl(const QVariantMap& args, const char* key,
                             double fallback)
{
    return args.value(QLatin1String(key), fallback).toDouble();
}

QString MapEditor::McpBridge::mcpArgStr(const QVariantMap& args, const char* key)
{
    return args.value(QLatin1String(key)).toString();
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

QVariantMap MapEditor::McpBridge::invoke(const QString& name, const QVariantMap& args)
{
    using Handler = QVariantMap (McpBridge::*)(const QString&, const QVariantMap&);
    static const Handler kHandlers[] = {
        &McpBridge::mcpHandleSession,
        &McpBridge::mcpHandleBattleMapFiles,
        &McpBridge::mcpHandleDifference,
        &McpBridge::mcpHandlePlayers,
        &McpBridge::mcpHandleObjects,
        &McpBridge::mcpHandleBrushes,
        &McpBridge::mcpHandleTerrain,
        &McpBridge::mcpHandleHistory,
    };
    for (Handler handler : kHandlers) {
        const QVariantMap result = (this->*handler)(name, args);
        if (!mcpIsUnhandled(result)) {
            return result;
        }
    }
    return mcpError(QStringLiteral("unhandled tool: %1").arg(name));
}

// ---------------------------------------------------------------------------
// Session / world
// ---------------------------------------------------------------------------

QVariantMap MapEditor::McpBridge::mcpHandleSession(const QString& name,
                                         const QVariantMap& args)
{
    static const QSet<QString> kTools = {
        QStringLiteral("set_game_path"), QStringLiteral("list_worlds"),
        QStringLiteral("get_world_properties"), QStringLiteral("create_world"),
        QStringLiteral("open_world"), QStringLiteral("save_world"),
    };
    if (!kTools.contains(name)) {
        return mcpUnhandled();
    }

    if (name == QLatin1String("set_game_path")) {
        const QString path = mcpArgStr(args, "path");
        if (path.isEmpty()) {
            return mcpError(QStringLiteral("path must not be empty"));
        }
        if (!QFileInfo::exists(path)) {
            return mcpError(QStringLiteral("directory does not exist: %1").arg(path));
        }
        if (!session_->setGameDir(path.toStdString())) {
            return mcpError(
                QStringLiteral("not a game folder (RESOURCE/Worlds missing): %1")
                    .arg(path));
        }
        worldUnloadHook_();
        if (!session_->scanWorlds()) {
            return mcpError(QStringLiteral(
                "game directory set, but no worlds found in RESOURCE/Worlds"));
        }
        if (battleMaps_->panel()) {
            battleMaps_->panel()->setTemplates(battleMaps_->templates());
        }
        statusBarHook_();
        return mcpOk({{QStringLiteral("worlds"),
                       static_cast<int>(session_->getWorldList().size())}});
    }

    if (name == QLatin1String("list_worlds")) {
        const QString dirError = mcpRequireGameDir();
        if (!dirError.isEmpty()) {
            return mcpError(dirError);
        }
        QVariantList worlds;
        for (const MapEditor::WorldInfo& info : session_->getWorldList()) {
            worlds.append(QVariantMap{
                {QStringLiteral("name"), QString::fromStdString(info.name)},
                {QStringLiteral("dirKey"), QString::fromStdString(info.dirKey)},
                {QStringLiteral("modName"), QString::fromStdString(info.modName)},
                {QStringLiteral("displayName"),
                 QString::fromStdString(info.displayName())},
            });
        }
        return mcpOk({{QStringLiteral("worlds"), worlds}});
    }

    if (name == QLatin1String("get_world_properties")) {
        const QString worldError = mcpRequireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        return mcpOk({
            {QStringLiteral("name"),
             QString::fromStdString(session_->currentWorldName())},
            {QStringLiteral("width"), static_cast<int>(session_->mapWidth())},
            {QStringLiteral("height"), static_cast<int>(session_->mapHeight())},
            {QStringLiteral("zeroLayerHeight"), session_->zeroLayerHeight()},
        });
    }

    if (name == QLatin1String("create_world")) {
        const QString dirError = mcpRequireGameDir();
        if (!dirError.isEmpty()) {
            return mcpError(dirError);
        }
        const std::string worldName = mcpArgStr(args, "name").toStdString();
        if (!MapEditor::isValidWorldName(worldName)) {
            return mcpError(QStringLiteral("invalid world name '%1'")
                                .arg(mcpArgStr(args, "name")));
        }
        MapEditor::NewWorldParams params;
        params.name = worldName;
        params.mapPowerX = params.mapPowerY = mcpArgInt(args, "mapPower", 11);
        params.hZeroPlast = mcpArgInt(args, "hZeroPlast", 30);
        params.geoLayerHeight = mcpArgInt(args, "geoLayerHeight", 20);
        params.flatSurface = args.value(QStringLiteral("flat"), true).toBool();
        const std::string templ = mcpArgStr(args, "templateWorld").toStdString();
        const MapEditor::WorldCreateResult result =
            MapEditor::createNewWorld(*session_, params, templ);
        if (!result.ok) {
            return mcpError(QStringLiteral("createNewWorld failed: %1")
                                .arg(QString::fromStdString(result.error)));
        }
        worldLoadedHook_();
        statusBarHook_();
        return mcpOk();
    }

    if (name == QLatin1String("open_world")) {
        const QString dirError = mcpRequireGameDir();
        if (!dirError.isEmpty()) {
            return mcpError(dirError);
        }
        const std::string worldName = mcpArgStr(args, "name").toStdString();
        if (!session_->loadWorldByName(worldName)) {
            return mcpError(QStringLiteral("failed to load world '%1'")
                                .arg(mcpArgStr(args, "name")));
        }
        worldLoadedHook_();
        statusBarHook_();
        return mcpOk();
    }

    // save_world
    const QString worldError = mcpRequireWorld();
    if (!worldError.isEmpty()) {
        return mcpError(worldError);
    }
    if (!session_->saveWorld()) {
        return mcpError(QStringLiteral("saveWorld failed"));
    }
    statusBarHook_();
    return mcpOk();
}

// ---------------------------------------------------------------------------
// Battle map files
// ---------------------------------------------------------------------------

QVariantMap MapEditor::McpBridge::mcpHandleBattleMapFiles(const QString& name,
                                                const QVariantMap& args)
{
    static const QSet<QString> kTools = {
        QStringLiteral("list_battle_maps"), QStringLiteral("open_battle_map"),
        QStringLiteral("save_battle_map"), QStringLiteral("new_battle_map"),
    };
    if (!kTools.contains(name)) {
        return mcpUnhandled();
    }

    if (name == QLatin1String("list_battle_maps")) {
        const QString dirError = mcpRequireGameDir();
        if (!dirError.isEmpty()) {
            return mcpError(dirError);
        }
        return mcpOk({{QStringLiteral("battleMaps"), battleMaps_->templates()}});
    }

    if (name == QLatin1String("open_battle_map")) {
        const QString dirError = mcpRequireGameDir();
        if (!dirError.isEmpty()) {
            return mcpError(dirError);
        }
        QString fileName = mcpArgStr(args, "name");
        if (fileName.isEmpty()) {
            return mcpError(QStringLiteral("name must not be empty"));
        }
        if (!fileName.endsWith(QLatin1String(".spg"), Qt::CaseInsensitive)) {
            fileName += QStringLiteral(".spg");
        }
        const QDir gameDir(QString::fromStdString(session_->getGameDir()));
        QString path;
        for (const QString& sub :
             {QStringLiteral("RESOURCE/Battle"), QStringLiteral("RESOURCE/Worlds")}) {
            const QString candidate = gameDir.filePath(sub + QStringLiteral("/") + fileName);
            if (QFileInfo::exists(candidate)) {
                path = candidate;
                break;
            }
        }
        if (path.isEmpty()) {
            return mcpError(QStringLiteral("battle map not found: %1").arg(fileName));
        }
        if (!battleMaps_->openPath(path)) {
            return mcpError(QStringLiteral("cannot read the battle map file"));
        }
        return mcpOk({{QStringLiteral("world"),
                       QString::fromStdString(
                           battleMaps_->spgFile().worldName())}});
    }

    if (name == QLatin1String("save_battle_map")) {
        const QString bmError = mcpRequireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        if (battleMaps_->path().isEmpty()) {
            return mcpError(QStringLiteral(
                "the battle map has no file path; use new_battle_map"));
        }
        if (!battleMaps_->saveToCurrentPath()) {
            return mcpError(QStringLiteral("failed to write the battle map"));
        }
        return mcpOk({{QStringLiteral("path"), battleMaps_->path()}});
    }

    // new_battle_map
    const QString worldError = mcpRequireWorld();
    if (!worldError.isEmpty()) {
        return mcpError(worldError);
    }
    const QString mapName = mcpArgStr(args, "name");
    const QString templ = mcpArgStr(args, "template").isEmpty()
                              ? battleMaps_->templates().value(0)
                              : mcpArgStr(args, "template");
    if (mapName.isEmpty() || templ.isEmpty()) {
        return mcpError(QStringLiteral("name and template are required"));
    }
    battleMaps_->newBattleMapFromTemplate(mapName, templ,
                                          mcpArgInt(args, "players", 2));
    if (!battleMaps_->isLoaded()) {
        return mcpError(QStringLiteral("new_battle_map failed"));
    }
    return mcpOk();
}

// ---------------------------------------------------------------------------
// Difference mode (.gmp)
// ---------------------------------------------------------------------------

QVariantMap MapEditor::McpBridge::mcpHandleDifference(const QString& name,
                                            const QVariantMap& args)
{
    static const QSet<QString> kTools = {
        QStringLiteral("set_difference_mode"),
        QStringLiteral("save_difference"),
        QStringLiteral("load_difference"),
    };
    if (!kTools.contains(name)) {
        return mcpUnhandled();
    }

    if (name == QLatin1String("set_difference_mode")) {
        const QString worldError = mcpRequireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        const bool enabled = args.value(QStringLiteral("enabled")).toBool();
        if (!session_->setDiffMode(enabled)) {
            return mcpError(QStringLiteral("setDiffMode(%1) failed")
                                .arg(enabled ? 1 : 0));
        }
        paletteRefreshHook_();
        return mcpOk({{QStringLiteral("diffMode"), session_->isDiffMode()},
                      {QStringLiteral("hasDiffData"), session_->hasDiffData()},
                      {QStringLiteral("unsavedDiffChanges"),
                       session_->hasUnsavedDiffChanges()},
                      {QStringLiteral("note"),
                       QStringLiteral("switching cleared the undo history")}});
    }

    // save_difference / load_difference share the path resolution.
    const QString worldError = mcpRequireWorld();
    if (!worldError.isEmpty()) {
        return mcpError(worldError);
    }
    const bool isSave = name == QLatin1String("save_difference");
    QString path = mcpArgStr(args, "file");
    if (path.isEmpty() && !battleMaps_->path().isEmpty()) {
        // The game looks for <map>.gmp next to <map>.spg.
        path = battleMaps_->path();
        path.replace(path.size() - 3, 3, QStringLiteral("gmp"));
    }
    if (path.isEmpty()) {
        return mcpError(QStringLiteral(
            "no 'file' argument and no battle map to derive the .gmp "
            "path from"));
    }
    if (!isSave && !QFileInfo::exists(path)) {
        return mcpError(QStringLiteral("no such file: %1").arg(path));
    }
    const bool ok =
        isSave ? session_->saveDiffToGmp(path.toStdString())
               : session_->loadDiffFromGmp(path.toStdString());
    if (!ok) {
        return mcpError(QStringLiteral("%1 failed: %2")
                            .arg(isSave ? QStringLiteral("saveDiffToGmp")
                                        : QStringLiteral("loadDiffFromGmp"),
                                 path));
    }
    if (!isSave) {
        paletteRefreshHook_();
    }
    return mcpOk({{QStringLiteral("path"), path},
                  {QStringLiteral("hasDiffData"), session_->hasDiffData()},
                  {QStringLiteral("diffMode"), session_->isDiffMode()}});
}

// ---------------------------------------------------------------------------
// Battle map players
// ---------------------------------------------------------------------------

QVariantMap MapEditor::McpBridge::mcpApplyPlayerArgs(int index, const QVariantMap& args)
{
    using MapEditor::SpgFile;
    SpgFile::PlayerData data = battleMaps_->spgFile().playerData(index);
    if (args.contains(QStringLiteral("name"))) {
        data.name = mcpArgStr(args, "name").toStdString();
    }
    if (args.contains(QStringLiteral("realPlayerType"))) {
        data.realPlayerType = mcpArgStr(args, "realPlayerType").toStdString();
    }
    if (args.contains(QStringLiteral("belligerent"))) {
        data.belligerent = mcpArgStr(args, "belligerent").toStdString();
    }
    if (args.contains(QStringLiteral("colorIndex"))) {
        data.colorIndex = mcpArgInt(args, "colorIndex");
    }
    if (args.contains(QStringLiteral("clan"))) {
        data.clan = mcpArgInt(args, "clan");
    }
    if (args.contains(QStringLiteral("difficulty"))) {
        data.difficulty = mcpArgStr(args, "difficulty").toStdString();
    }
    if (args.contains(QStringLiteral("handicap"))) {
        data.handicap = mcpArgInt(args, "handicap");
    }
    if (!battleMaps_->spgFile().setPlayerData(index, data)) {
        return mcpError(QStringLiteral("setPlayerData failed"));
    }
    if (args.contains(QStringLiteral("x")) || args.contains(QStringLiteral("y"))) {
        double oldX = 0;
        double oldY = 0;
        battleMaps_->spgFile().playerAnchor(index, oldX, oldY);
        const double newX = mcpArgDbl(args, "x", oldX);
        const double newY = mcpArgDbl(args, "y", oldY);
        double dz = 0;
        bool canMove = true;
        if (session_->isWorldLoaded()) {
            const int oldTerrain =
                session_->terrainHeightAt(qRound(oldX), qRound(oldY));
            const int newTerrain =
                session_->terrainHeightAt(qRound(newX), qRound(newY));
            if (oldTerrain < 0 || newTerrain < 0) {
                canMove = false;
            } else {
                dz = static_cast<double>(newTerrain - oldTerrain);
            }
        }
        if (!canMove ||
            !battleMaps_->spgFile().setPlayerAnchor(index, newX, newY, dz)) {
            return mcpError(QStringLiteral(
                "failed to move the spawn group - the location was not "
                "changed"));
        }
    }
    battleMaps_->syncSpawnOverlay();
    if (battleMaps_->panel()) {
        battleMaps_->panel()->refresh();
    }
    return QVariantMap();
}

QVariantMap MapEditor::McpBridge::mcpHandlePlayers(const QString& name,
                                         const QVariantMap& args)
{
    static const QSet<QString> kTools = {
        QStringLiteral("list_players"), QStringLiteral("add_player"),
        QStringLiteral("update_player"), QStringLiteral("remove_player"),
    };
    if (!kTools.contains(name)) {
        return mcpUnhandled();
    }
    if (name == QLatin1String("list_players")) {
        const QString bmError = mcpRequireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        QVariantList list;
        const int count = battleMaps_->spgFile().playerCount();
        for (int i = 0; i < count; ++i) {
            const MapEditor::SpgFile::PlayerData data = battleMaps_->spgFile().playerData(i);
            double ax = 0;
            double ay = 0;
            battleMaps_->spgFile().playerAnchor(i, ax, ay);
            list.append(QVariantMap{
                {QStringLiteral("index"), i},
                {QStringLiteral("playerID"), data.playerID},
                {QStringLiteral("name"), QString::fromStdString(data.name)},
                {QStringLiteral("realPlayerType"),
                 QString::fromStdString(data.realPlayerType)},
                {QStringLiteral("belligerent"),
                 QString::fromStdString(data.belligerent)},
                {QStringLiteral("colorIndex"), data.colorIndex},
                {QStringLiteral("clan"), data.clan},
                {QStringLiteral("difficulty"),
                 QString::fromStdString(data.difficulty)},
                {QStringLiteral("handicap"), data.handicap},
                {QStringLiteral("x"), ax},
                {QStringLiteral("y"), ay},
            });
        }
        return mcpOk({{QStringLiteral("players"), list}});
    }

    if (name == QLatin1String("add_player")) {
        const QString bmError = mcpRequireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        const int count = battleMaps_->spgFile().playerCount();
        if (count >= 8) {
            return mcpError(QStringLiteral(
                "a battle map holds at most 8 players"));
        }
        // Grow by cloning the last player (the setPlayerCount grow path).
        if (!battleMaps_->spgFile().setPlayerCount(count + 1)) {
            return mcpError(QStringLiteral("setPlayerCount failed"));
        }
        const int index = count; // the new last player
        const QVariantMap applied = mcpApplyPlayerArgs(index, args);
        if (!applied.value(QStringLiteral("error")).toString().isEmpty()) {
            return applied;
        }
        return mcpOk({{QStringLiteral("index"), index},
                      {QStringLiteral("players"), battleMaps_->spgFile().playerCount()}});
    }

    if (name == QLatin1String("update_player")) {
        const QString bmError = mcpRequireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        const int index = mcpArgInt(args, "index", -1);
        if (index < 0 || index >= battleMaps_->spgFile().playerCount()) {
            return mcpError(QStringLiteral("no player at index %1").arg(index));
        }
        const QVariantMap applied = mcpApplyPlayerArgs(index, args);
        if (!applied.value(QStringLiteral("error")).toString().isEmpty()) {
            return applied;
        }
        return mcpOk();
    }

    // remove_player
    const QString bmError = mcpRequireBattleMap();
    if (!bmError.isEmpty()) {
        return mcpError(bmError);
    }
    const int index = mcpArgInt(args, "index", -1);
    if (index < 0 || index >= battleMaps_->spgFile().playerCount()) {
        return mcpError(QStringLiteral("no player at index %1").arg(index));
    }
    if (!battleMaps_->spgFile().removePlayer(index)) {
        return mcpError(QStringLiteral(
            "removePlayer failed (invalid index or the last player?)"));
    }
    battleMaps_->refreshUI();
    return mcpOk({{QStringLiteral("players"), battleMaps_->spgFile().playerCount()}});
}

// ---------------------------------------------------------------------------
// Battle map objects
// ---------------------------------------------------------------------------

QVariantMap MapEditor::McpBridge::mcpHandleObjects(const QString& name,
                                         const QVariantMap& args)
{
    static const QSet<QString> kTools = {
        QStringLiteral("list_battle_map_objects"),
        QStringLiteral("place_building"), QStringLiteral("update_building"),
        QStringLiteral("delete_building"),
        QStringLiteral("place_scum_spot"), QStringLiteral("update_scum_spot"),
        QStringLiteral("delete_scum_spot"),
        QStringLiteral("place_nature_object"),
        QStringLiteral("update_nature_object"),
        QStringLiteral("delete_nature_object"),
    };
    if (!kTools.contains(name)) {
        return mcpUnhandled();
    }
    const QString bmError = mcpRequireBattleMap();
    if (!bmError.isEmpty()) {
        return mcpError(bmError);
    }

    if (name == QLatin1String("list_battle_map_objects")) {
        QVariantList buildings;
        std::vector<MapEditor::SpgFile::PlacedBuilding> list = battleMaps_->spgFile().buildings();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const MapEditor::BuildingInfo* info =
                MapEditor::buildingInfo(list[i].attributeId);
            buildings.append(QVariantMap{
                {QStringLiteral("index"), static_cast<int>(i)},
                {QStringLiteral("unitId"), list[i].unitId},
                {QStringLiteral("attributeId"), list[i].attributeId},
                {QStringLiteral("type"), info ? QString::fromLatin1(info->identifier) : QString()},
                {QStringLiteral("x"), list[i].x},
                {QStringLiteral("y"), list[i].y},
                {QStringLiteral("angleDeg"), list[i].angleDeg},
                {QStringLiteral("hp"), list[i].hp},
            });
        }
        QVariantList scum;
        std::vector<MapEditor::SpgFile::ScumSpot> nests = battleMaps_->spgFile().scumSpots();
        for (std::size_t i = 0; i < nests.size(); ++i) {
            const MapEditor::ScumSpotInfo* info =
                MapEditor::scumSpotInfo(nests[i].attributeId);
            scum.append(QVariantMap{
                {QStringLiteral("index"), static_cast<int>(i)},
                {QStringLiteral("attributeId"), nests[i].attributeId},
                {QStringLiteral("type"), info ? QString::fromLatin1(info->identifier) : QString()},
                {QStringLiteral("x"), nests[i].x},
                {QStringLiteral("y"), nests[i].y},
                {QStringLiteral("angleDeg"), nests[i].angleDeg},
                {QStringLiteral("hp"), nests[i].hp},
                {QStringLiteral("paired"), nests[i].paired},
                {QStringLiteral("label"), QString::fromStdString(nests[i].label)},
                {QStringLiteral("filthType"), nests[i].filth.filthType},
                {QStringLiteral("creatureNum"), nests[i].filth.creatureNum},
                {QStringLiteral("sleep"), nests[i].filth.sleep},
                {QStringLiteral("sleepPeriod"), nests[i].filth.sleepPeriod},
                {QStringLiteral("attackPeriod"), nests[i].filth.attackPeriond},
                {QStringLiteral("activatingUnit"), nests[i].filth.activatingUnit},
                {QStringLiteral("activatingDistance"), nests[i].filth.activatingDistance},
                {QStringLiteral("attackPlayer"), nests[i].filth.attackPlayer},
            });
        }
        QVariantList nature;
        std::vector<MapEditor::SpgFile::NatureObject> objects =
            battleMaps_->spgFile().natureObjects();
        for (std::size_t i = 0; i < objects.size(); ++i) {
            nature.append(QVariantMap{
                {QStringLiteral("index"), static_cast<int>(i)},
                {QStringLiteral("model"), QString::fromStdString(objects[i].modelName)},
                {QStringLiteral("x"), objects[i].x},
                {QStringLiteral("y"), objects[i].y},
                {QStringLiteral("z"), objects[i].z},
                {QStringLiteral("angleDeg"), objects[i].angleDeg},
                {QStringLiteral("radius"), objects[i].radius},
                {QStringLiteral("destroy"), objects[i].destroyable},
                {QStringLiteral("collisions"), objects[i].collisions},
                {QStringLiteral("lighting"), objects[i].lighting},
            });
        }
        return mcpOk({{QStringLiteral("buildings"), buildings},
                      {QStringLiteral("scumSpots"), scum},
                      {QStringLiteral("natureObjects"), nature}});
    }

    if (name == QLatin1String("place_building")) {
        const int attributeId = mcpArgInt(args, "attributeId", -1);
        const MapEditor::BuildingInfo* info = MapEditor::buildingInfo(attributeId);
        if (!info) {
            return mcpError(QStringLiteral("attributeId must be 0..42, got %1")
                                .arg(attributeId));
        }
        const double x = mcpArgDbl(args, "x");
        const double y = mcpArgDbl(args, "y");
        if (battleMaps_->buildingPlacementBlocked(x, y, info->radius)) {
            return mcpError(QStringLiteral(
                "the placement intersects another building"));
        }
        MapEditor::SpgFile::PlacedBuilding b;
        b.unitId = battleMaps_->spgFile().maxUnitId() + 1;
        b.attributeId = attributeId;
        b.x = x;
        b.y = y;
        b.z = session_->zeroLayerHeight() + info->zOffset;
        b.angleDeg = mcpArgDbl(args, "angle", 0.0);
        b.hp = mcpArgInt(args, "hp", 100);
        if (!battleMaps_->spgFile().addBuilding(b)) {
            return mcpError(QStringLiteral("addBuilding failed"));
        }
        battleMaps_->reloadPlacedBuildings();
        return mcpOk({{QStringLiteral("unitId"), b.unitId}});
    }

    if (name == QLatin1String("update_building")) {
        battleMaps_->reloadPlacedBuildings();
        const int row = mcpArgInt(args, "index", -1);
        if (row < 0 || row >= static_cast<int>(battleMaps_->placedBuildings().size())) {
            return mcpError(QStringLiteral("no building at index %1").arg(row));
        }
        const MapEditor::SpgFile::PlacedBuilding& b =
            battleMaps_->placedBuildings()[static_cast<std::size_t>(row)];
        if (!battleMaps_->spgFile().setBuildingPose(
                b.index, mcpArgDbl(args, "x", b.x), mcpArgDbl(args, "y", b.y),
                mcpArgDbl(args, "angle", b.angleDeg))) {
            return mcpError(QStringLiteral("setBuildingPose failed"));
        }
        if (args.contains(QStringLiteral("hp")) &&
            !battleMaps_->spgFile().setBuildingHp(b.index, mcpArgInt(args, "hp"))) {
            return mcpError(QStringLiteral("setBuildingHp failed"));
        }
        battleMaps_->reloadPlacedBuildings();
        return mcpOk();
    }

    if (name == QLatin1String("delete_building")) {
        battleMaps_->reloadPlacedBuildings();
        const int row = mcpArgInt(args, "index", -1);
        if (row < 0 || row >= static_cast<int>(battleMaps_->placedBuildings().size())) {
            return mcpError(QStringLiteral("no building at index %1").arg(row));
        }
        if (!battleMaps_->spgFile().removeBuilding(battleMaps_->placedBuildings()[static_cast<std::size_t>(row)].index)) {
            return mcpError(QStringLiteral("removeBuilding failed"));
        }
        battleMaps_->reloadPlacedBuildings();
        return mcpOk();
    }

    if (name == QLatin1String("place_scum_spot")) {
        const int attributeId = mcpArgInt(args, "attributeId", -1);
        const MapEditor::ScumSpotInfo* info = MapEditor::scumSpotInfo(attributeId);
        if (!info) {
            return mcpError(QStringLiteral("attributeId must be 43..46, got %1")
                                .arg(attributeId));
        }
        const double x = mcpArgDbl(args, "x");
        const double y = mcpArgDbl(args, "y");
        for (const MapEditor::SpgFile::ScumSpot& s : battleMaps_->spgFile().scumSpots()) {
            if (std::hypot(s.x - x, s.y - y) < MapEditor::kMinScumSpotDistance) {
                return mcpError(QStringLiteral(
                    "scum nests must be at least %1 units apart")
                                    .arg(MapEditor::kMinScumSpotDistance));
            }
        }
 
        MapEditor::SpgFile::ScumSpot spot;
        spot.attributeId = attributeId;
        spot.x = x;
        spot.y = y;
        spot.z = session_->zeroLayerHeight() + info->zOffset;
        spot.angleDeg = mcpArgDbl(args, "angle", 0.0);
        spot.hp = 100;
        spot.filth.filthType = mcpArgInt(args, "filthType", 2);
        spot.filth.creatureNum = mcpArgInt(args, "creatureNum", 10);
        if (!battleMaps_->spgFile().addScumSpot(spot)) {
            return mcpError(QStringLiteral("addScumSpot failed"));
        }
        battleMaps_->reloadScumSpots();
        return mcpOk({{QStringLiteral("label"), QString::fromStdString(spot.label)}});
    }

    if (name == QLatin1String("update_scum_spot")) {
        const std::vector<MapEditor::SpgFile::ScumSpot> nests = battleMaps_->spgFile().scumSpots();
        const int row = mcpArgInt(args, "index", -1);
        if (row < 0 || row >= static_cast<int>(nests.size())) {
            return mcpError(QStringLiteral("no scum spot at index %1").arg(row));
        }
        const MapEditor::SpgFile::ScumSpot& s = nests[static_cast<std::size_t>(row)];
        if (args.contains(QStringLiteral("x")) ||
            args.contains(QStringLiteral("y")) ||
            args.contains(QStringLiteral("angle"))) {
            if (!battleMaps_->spgFile().setScumSpotPose(
                    s.index, mcpArgDbl(args, "x", s.x), mcpArgDbl(args, "y", s.y),
                    mcpArgDbl(args, "angle", s.angleDeg))) {
                return mcpError(QStringLiteral("setScumSpotPose failed"));
            }
        }
        MapEditor::SpgFile::FilthParams filth = s.filth;
        if (args.contains(QStringLiteral("filthType"))) {
            filth.filthType = mcpArgInt(args, "filthType");
        }
        if (args.contains(QStringLiteral("creatureNum"))) {
            filth.creatureNum = mcpArgInt(args, "creatureNum");
        }
        if (args.contains(QStringLiteral("sleep"))) {
            filth.sleep = args.value(QStringLiteral("sleep")).toBool();
        }
        if (args.contains(QStringLiteral("sleepPeriod"))) {
            filth.sleepPeriod = static_cast<float>(mcpArgDbl(args, "sleepPeriod"));
        }
        if (args.contains(QStringLiteral("attackPeriod"))) {
            filth.attackPeriond = static_cast<float>(mcpArgDbl(args, "attackPeriod"));
        }
        if (args.contains(QStringLiteral("activatingUnit"))) {
            filth.activatingUnit = mcpArgInt(args, "activatingUnit");
        }
        if (args.contains(QStringLiteral("activatingDistance"))) {
            filth.activatingDistance = static_cast<float>(mcpArgDbl(args, "activatingDistance"));
        }
        if (args.contains(QStringLiteral("attackPlayer"))) {
            filth.attackPlayer = mcpArgInt(args, "attackPlayer");
        }
        if (!battleMaps_->spgFile().setScumSpotFilth(s.index, filth)) {
            return mcpError(QStringLiteral("setScumSpotFilth failed"));
        }
        battleMaps_->reloadScumSpots();
        return mcpOk();
    }

    if (name == QLatin1String("delete_scum_spot")) {
        const std::vector<MapEditor::SpgFile::ScumSpot> nests = battleMaps_->spgFile().scumSpots();
        const int row = mcpArgInt(args, "index", -1);
        if (row < 0 || row >= static_cast<int>(nests.size())) {
            return mcpError(QStringLiteral("no scum spot at index %1").arg(row));
        }
        if (!battleMaps_->spgFile().removeScumSpot(nests[static_cast<std::size_t>(row)].index)) {
            return mcpError(QStringLiteral("removeScumSpot failed"));
        }
        battleMaps_->reloadScumSpots();
        return mcpOk();
    }

    if (name == QLatin1String("place_nature_object")) {
        const QString model = mcpArgStr(args, "model");
        if (model.isEmpty()) {
            return mcpError(QStringLiteral("model must not be empty"));
        }
        MapEditor::SpgFile::NatureObject object;
        object.modelName = model.toStdString();
        object.x = mcpArgDbl(args, "x");
        object.y = mcpArgDbl(args, "y");
        object.z = args.contains(QStringLiteral("z"))
                       ? mcpArgDbl(args, "z")
                       : session_->zeroLayerHeight();
        object.angleDeg = mcpArgDbl(args, "angle", 0.0);
        object.radius = mcpArgDbl(args, "radius", 10.0);
        object.destroyable = args.value(QStringLiteral("destroy"), true).toBool();
        object.collisions = args.value(QStringLiteral("collisions"), true).toBool();
        object.lighting = args.value(QStringLiteral("lighting"), false).toBool();
        if (!battleMaps_->spgFile().addNatureObject(object)) {
            return mcpError(QStringLiteral("addNatureObject failed"));
        }
        battleMaps_->reloadNatureObjects();
        return mcpOk({{QStringLiteral("unitId"), object.unitId}});
    }

    if (name == QLatin1String("update_nature_object")) {
        const std::vector<MapEditor::SpgFile::NatureObject> objects =
            battleMaps_->spgFile().natureObjects();
        const int row = mcpArgInt(args, "index", -1);
        if (row < 0 || row >= static_cast<int>(objects.size())) {
            return mcpError(QStringLiteral("no nature object at index %1").arg(row));
        }
        const MapEditor::SpgFile::NatureObject& n = objects[static_cast<std::size_t>(row)];
        if (args.contains(QStringLiteral("x")) ||
            args.contains(QStringLiteral("y")) ||
            args.contains(QStringLiteral("z")) ||
            args.contains(QStringLiteral("angle"))) {
            if (!battleMaps_->spgFile().setNatureObjectPose(
                    n.index, mcpArgDbl(args, "x", n.x), mcpArgDbl(args, "y", n.y),
                    mcpArgDbl(args, "z", n.z), mcpArgDbl(args, "angle", n.angleDeg))) {
                return mcpError(QStringLiteral("setNatureObjectPose failed"));
            }
        }
        if (args.contains(QStringLiteral("radius")) ||
            args.contains(QStringLiteral("destroy")) ||
            args.contains(QStringLiteral("collisions")) ||
            args.contains(QStringLiteral("lighting"))) {
            if (!battleMaps_->spgFile().setNatureObjectShape(
                    n.index, mcpArgDbl(args, "radius", n.radius),
                    args.value(QStringLiteral("destroy"), n.destroyable).toBool(),
                    args.value(QStringLiteral("collisions"), n.collisions).toBool(),
                    args.value(QStringLiteral("lighting"), n.lighting).toBool())) {
                return mcpError(QStringLiteral("setNatureObjectShape failed"));
            }
        }
        battleMaps_->reloadNatureObjects();
        return mcpOk();
    }

    // delete_nature_object
    const std::vector<MapEditor::SpgFile::NatureObject> objects =
        battleMaps_->spgFile().natureObjects();
    const int row = mcpArgInt(args, "index", -1);
    if (row < 0 || row >= static_cast<int>(objects.size())) {
        return mcpError(QStringLiteral("no nature object at index %1").arg(row));
    }
    if (!battleMaps_->spgFile().removeNatureObject(objects[static_cast<std::size_t>(row)].index)) {
        return mcpError(QStringLiteral("removeNatureObject failed"));
    }
    battleMaps_->reloadNatureObjects();
    return mcpOk();
}

// ---------------------------------------------------------------------------
// Brushes (apply_brush, decal/model loaders, scripts)
// ---------------------------------------------------------------------------

QVariantMap MapEditor::McpBridge::mcpHandleBrushes(const QString& name,
                                         const QVariantMap& args)
{
    static const QSet<QString> kTools = {
        QStringLiteral("apply_brush"), QStringLiteral("load_brush_image"),
        QStringLiteral("load_decal_image"),
        QStringLiteral("import_model"), QStringLiteral("run_script"),
        QStringLiteral("set_scripted_brush_script"),
    };
    if (!kTools.contains(name)) {
        return mcpUnhandled();
    }
    using MapEditor::EditorSession;

    if (name == QLatin1String("load_brush_image")) {
        const QString worldError = mcpRequireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        const QString filePath = mcpArgStr(args, "file");
        if (filePath.isEmpty()) {
            return mcpError(QStringLiteral("file must not be empty"));
        }
        // The BrushUiController owns the mask original + scale state, so the
        // UI spinbox and the MCP tool stay in sync (same core as the toolbar
        // Load Brush Image button).
        const QString error = brushUI_->loadBrushImage(filePath);
        if (!error.isEmpty()) {
            return mcpError(error);
        }
        const MapEditor::BrushMask& mask = brushTool_->imageMask();
        return mcpOk({{QStringLiteral("width"), mask.sx},
                      {QStringLiteral("height"), mask.sy},
                      {QStringLiteral("maskScale"),
                       brushUI_->maskScalePercent()}});
    }

    if (name == QLatin1String("load_decal_image")) {
        const QString worldError = mcpRequireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        const QString filePath = mcpArgStr(args, "file");
        QImageReader reader(filePath);
        reader.setAutoTransform(true);
        const QImage img = reader.read();
        if (img.isNull()) {
            return mcpError(QStringLiteral("cannot load image '%1': %2")
                                .arg(filePath, reader.errorString()));
        }
        if (img.width() > MapEditor::ImageDecal::kMaxImageSize ||
            img.height() > MapEditor::ImageDecal::kMaxImageSize) {
            return mcpError(QStringLiteral(
                "the image is too large (%1x%2 px; max %3 px per side)")
                                .arg(img.width()).arg(img.height())
                                .arg(MapEditor::ImageDecal::kMaxImageSize));
        }
        const QImage rgbaImage = img.convertToFormat(QImage::Format_ARGB32);
        const int width = rgbaImage.width();
        const int height = rgbaImage.height();
        std::vector<unsigned char> rgba(
            static_cast<std::size_t>(width) * height * 4, 0);
        for (int py = 0; py < height; ++py) {
            const QRgb* line =
                reinterpret_cast<const QRgb*>(rgbaImage.constScanLine(py));
            for (int px = 0; px < width; ++px) {
                const std::size_t base =
                    (static_cast<std::size_t>(py) * width + px) * 4;
                rgba[base + 0] = static_cast<unsigned char>(qRed(line[px]));
                rgba[base + 1] = static_cast<unsigned char>(qGreen(line[px]));
                rgba[base + 2] = static_cast<unsigned char>(qBlue(line[px]));
                rgba[base + 3] = static_cast<unsigned char>(qAlpha(line[px]));
            }
        }
        const std::vector<unsigned char> palette =
            session_->paletteBytes(EditorSession::PaletteLayer::Dam);
        const std::vector<unsigned char> usage = session_->damPaletteUsage();
        const MapEditor::ImageDecal::PrepareResult prepared =
            MapEditor::ImageDecal::prepare(rgba.data(), width, height,
                                           palette.data(), usage.data());
        if (!prepared.ok) {
            return mcpError(QStringLiteral("image decal: %1")
                                .arg(QString::fromStdString(prepared.error)));
        }
        for (const MapEditor::ImageDecal::PaletteWrite& write :
             prepared.paletteWrites) {
            session_->setPaletteRgb(EditorSession::PaletteLayer::Dam,
                                    write.index, write.rgb);
        }
        session_->applyPalettes();
        paletteRefreshHook_();
        brushTool_->setDecalImage(prepared.image);
        return mcpOk({{QStringLiteral("width"), width},
                      {QStringLiteral("height"), height},
                      {QStringLiteral("paletteWrites"),
                       static_cast<int>(prepared.paletteWrites.size())}});
    }

    if (name == QLatin1String("import_model")) {
        const QString filePath = mcpArgStr(args, "file");
        const MapEditor::ModelBrush::ImportResult imported =
            MapEditor::ModelBrush::importPly(
                std::filesystem::u8path(filePath.toStdString()));
        if (!imported.ok) {
            return mcpError(
                QStringLiteral("Unable to import from PLY the model %1: %2")
                    .arg(QFileInfo(filePath).fileName(),
                         QString::fromStdString(imported.error)));
        }
        brushTool_->setModelBrushData(imported.model,
                                      MapEditor::ImageDecal::DecalImage());
        const bool quantized = brushUI_->quantizeModelBrush();
        return mcpOk({{QStringLiteral("width"), imported.model.width},
                      {QStringLiteral("height"), imported.model.height},
                      {QStringLiteral("quantized"), quantized}});
    }

    if (name == QLatin1String("run_script")) {
        const QString worldError = mcpRequireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        const std::string script = mcpArgStr(args, "script").toStdString();
        if (script.empty()) {
            return mcpError(QStringLiteral("script must not be empty"));
        }
        std::string errorOut;
        if (!MapEditor::runEditorScript(*session_, *brushTool_, brushTool_->params(),
                                        0, 0, script, errorOut)) {
            return mcpError(QString::fromStdString(errorOut));
        }
        return mcpOk();
    }

    if (name == QLatin1String("set_scripted_brush_script")) {
        const QString worldError = mcpRequireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        MapEditor::BrushParams params = brushTool_->params();
        params.script = mcpArgStr(args, "script").toStdString();
        brushTool_->setParams(params);
        return mcpOk();
    }

    return mcpApplyBrush(args);
}

QVariantMap MapEditor::McpBridge::mcpApplyBrush(const QVariantMap& args)
{
    using MapEditor::EditorSession;
    const QString worldError = mcpRequireWorld();
    if (!worldError.isEmpty()) {
        return mcpError(worldError);
    }
    // Scale of the loaded brush-image mask, like the toolbar Mask spin
    // (percent of the original image size, 10..800): the mask is recomputed
    // from the un-scaled original, so stamps never accumulate blur.
    if (args.contains(QStringLiteral("maskScale"))) {
        brushUI_->setMaskScalePercent(mcpArgInt(args, "maskScale", 100));
    }
    const QString effectName = mcpArgStr(args, "effect");
    const int x = mcpArgInt(args, "x");
    const int y = mcpArgInt(args, "y");
    // Image decal / model stamps own their geometry (the loaded image /
    // the scaled model rectangle) - route them through applyStamp like
    // the UI instead of building a shape mask. Dam layer only.
    if (effectName == QLatin1String("imageDecal") ||
        effectName == QLatin1String("model")) {
        MapEditor::BrushParams op = brushTool_->params();
        op.useImageMask = false;
        op.shape3d = MapEditor::Shape3DKind::None;
        op.modelBrush = false;
        op.mode = MapEditor::BrushMode::Dam;
        op.effect = MapEditor::BrushEffect::ChangeHeight;
        op.direction = 1;
        if (effectName == QLatin1String("imageDecal")) {
            if (brushTool_->decalImage().empty()) {
                return mcpError(QStringLiteral(
                    "no decal image loaded (load_decal_image first)"));
            }
            op.effect = MapEditor::BrushEffect::ImageDecal;
        } else {
            if (!brushTool_->hasModelBrushData()) {
                return mcpError(QStringLiteral(
                    "no model imported (import_model first)"));
            }
            op.modelBrush = true;
            op.modelMode = std::clamp(mcpArgInt(args, "mode", 0), 0, 2);
            op.modelHeight = mcpArgInt(args, "modelHeight", 32);
            op.modelScaleX = std::clamp(mcpArgDbl(args, "scaleX", 1.0), 0.0, 1.0);
            op.modelScaleY = std::clamp(mcpArgDbl(args, "scaleY", 1.0), 0.0, 1.0);
        }
        brushTool_->setParams(op);
        if (!brushTool_->applyStamp(x, y)) {
            return mcpError(QStringLiteral(
                "the stamp was refused (Dam layer only; is the image/"
                "model loaded?)"));
        }
        return mcpOk();
    }
    int radius = mcpArgInt(args, "radius", 1);
    if (radius < 1) {
        return mcpError(QStringLiteral("radius must be >= 1"));
    }
    radius = MapEditor::BrushTool::clampRadius(radius);
    const bool square = args.value(QStringLiteral("square"), false).toBool();

    MapEditor::BrushMask mask;
    const int sides = mcpArgInt(args, "sides", 0);
    if (sides >= MapEditor::kMinPolygonSides) {
        mask = MapEditor::BrushMasks::filledPolygon(
            radius, sides, mcpArgDbl(args, "angleOffset", 0.0));
    } else {
        mask = square ? MapEditor::BrushMasks::filledSquare(radius)
                      : MapEditor::BrushMasks::filledCircle(radius);
    }
    mask.translate(x, y);

    MapEditor::BrushParams op = brushTool_->params();
    op.useImageMask = false;
    op.shape = square ? MapEditor::BrushShape::FilledSquare
                      : MapEditor::BrushShape::FilledCircle;
    op.radius = radius;
    const QString layer = mcpArgStr(args, "layer").isEmpty()
                              ? QStringLiteral("dam")
                              : mcpArgStr(args, "layer");
    op.mode = layer == QLatin1String("geo") ? MapEditor::BrushMode::Geo
                                            : MapEditor::BrushMode::Dam;
    const auto setSculpt = [&]() {
        op.mode = MapEditor::BrushMode::Dam; // sculpting owns the dam layer
        op.sculptEffect = MapEditor::BrushTool::clampSculptEffect(
            mcpArgDbl(args, "fraction", 1.0));
    };
    const bool geoRequested = op.mode == MapEditor::BrushMode::Geo;

    if (effectName == QLatin1String("raiseHeight")) {
        op.effect = MapEditor::BrushEffect::ChangeHeight;
        op.direction = 1;
        op.strength = MapEditor::BrushTool::clampStrength(mcpArgDbl(args, "height", 1.0));
    } else if (effectName == QLatin1String("lowerHeight")) {
        op.effect = MapEditor::BrushEffect::ChangeHeight;
        op.direction = -1;
        op.strength = MapEditor::BrushTool::clampStrength(mcpArgDbl(args, "height", 1.0));
    } else if (effectName == QLatin1String("setHeight")) {
        op.effect = MapEditor::BrushEffect::AlignVariableH;
        op.heightLevel = MapEditor::BrushTool::clampHeightLevel(mcpArgDbl(args, "height", 0.0));
    } else if (effectName == QLatin1String("setToZP")) {
        op.effect = MapEditor::BrushEffect::Align4ZP;
    } else if (effectName == QLatin1String("paintSoot")) {
        op.effect = MapEditor::BrushEffect::PlotSoot;
    } else if (effectName == QLatin1String("paintVeryLightDam")) {
        op.effect = MapEditor::BrushEffect::VeryLightDam;
    } else if (effectName == QLatin1String("paintColor")) {
        op.effect = MapEditor::BrushEffect::SetColor;
        int colorIndex = mcpArgInt(args, "colorIndex", -1);
        if (colorIndex < 0 && args.contains(QStringLiteral("colorR"))) {
            // Nearest entry of the layer palette, like the JS paintColor.
            const int r = mcpArgInt(args, "colorR");
            const int g = mcpArgInt(args, "colorG");
            const int b = mcpArgInt(args, "colorB");
            int best = 0;
            long bestDist = LONG_MAX;
            for (int i = 0; i < 256; ++i) {
                const unsigned rgb =
                    session_->paletteRgb(EditorSession::PaletteLayer::Dam, i);
                const long dr = r - static_cast<int>((rgb >> 16) & 0xFF);
                const long dg = g - static_cast<int>((rgb >> 8) & 0xFF);
                const long db = b - static_cast<int>(rgb & 0xFF);
                const long dist = dr * dr + dg * dg + db * db;
                if (dist < bestDist) {
                    bestDist = dist;
                    best = i;
                }
            }
            colorIndex = best;
        }
        if (colorIndex < 0 || colorIndex > 255) {
            return mcpError(QStringLiteral(
                "paintColor needs colorIndex (0..255) or colorR/colorG/colorB"));
        }
        op.colorIndex = colorIndex;
    } else if (effectName == QLatin1String("smooth")) {
        op.effect = MapEditor::BrushEffect::Smooth;
        setSculpt();
    } else if (effectName == QLatin1String("flatten")) {
        op.effect = MapEditor::BrushEffect::Flatten;
        setSculpt();
    } else if (effectName == QLatin1String("tricubic")) {
        op.effect = MapEditor::BrushEffect::Tricubic;
        setSculpt();
    } else if (effectName == QLatin1String("inflate")) {
        op.effect = MapEditor::BrushEffect::Inflate;
        setSculpt();
    } else if (effectName == QLatin1String("gaussianBlur")) {
        op.effect = MapEditor::BrushEffect::GaussianBlur;
        setSculpt();
        op.blurRadius = MapEditor::BrushTool::clampBlurRadius(
            mcpArgInt(args, "blurRadius", 2), radius);
    } else {
        return mcpError(QStringLiteral("unknown effect '%1'").arg(effectName));
    }
    if (geoRequested && op.mode == MapEditor::BrushMode::Dam &&
        (effectName == QLatin1String("smooth") ||
         effectName == QLatin1String("flatten") ||
         effectName == QLatin1String("tricubic") ||
         effectName == QLatin1String("inflate") ||
         effectName == QLatin1String("gaussianBlur"))) {
        return mcpError(QStringLiteral(
            "sculpting effects (smooth/flatten/tricubic/inflate/"
            "gaussianBlur) are Dam-layer only"));
    }
    // 3D solid brushes (cone/pyramid/truncated*/torus/halfEllipsoid):
    // the shape replaces the flat stamp; only the height effects make
    // sense as the motion (raise/lower directional, setHeight absolute
    // base), exactly like the sculpting toolbar.
    const QString shapeName = mcpArgStr(args, "shape");
    if (!shapeName.isEmpty()) {
        const bool isHeightEffect =
            effectName == QLatin1String("raiseHeight") ||
            effectName == QLatin1String("lowerHeight") ||
            effectName == QLatin1String("setHeight");
        if (!isHeightEffect) {
            return mcpError(QStringLiteral(
                "a 3D shape requires effect raiseHeight, lowerHeight or "
                "setHeight"));
        }
        static const QHash<QString, MapEditor::Shape3DKind> shapes = {
            {QStringLiteral("cone"), MapEditor::Shape3DKind::Cone},
            {QStringLiteral("pyramid"), MapEditor::Shape3DKind::Pyramid},
            {QStringLiteral("truncatedCone"),
             MapEditor::Shape3DKind::TruncatedCone},
            {QStringLiteral("truncatedPyramid"),
             MapEditor::Shape3DKind::TruncatedPyramid},
            {QStringLiteral("torus"), MapEditor::Shape3DKind::Torus},
            {QStringLiteral("halfEllipsoid"),
             MapEditor::Shape3DKind::HalfEllipsoid},
        };
        const auto shapeIt = shapes.constFind(shapeName);
        if (shapeIt == shapes.constEnd()) {
            return mcpError(QStringLiteral(
                "unknown shape '%1' (cone, pyramid, truncatedCone, "
                "truncatedPyramid, torus, halfEllipsoid)")
                                .arg(shapeName));
        }
        op.mode = MapEditor::BrushMode::Dam; // 3D shapes own the dam layer
        op.shape3d = shapeIt.value();
        op.shapeHeight = mcpArgInt(args, "shapeHeight", 32);
        op.shapeTopOffsetX = mcpArgDbl(args, "topOffsetX", 0.0);
        op.shapeTopOffsetY = mcpArgDbl(args, "topOffsetY", 0.0);
        op.shapeTopRadius = mcpArgDbl(args, "topRadius", 0.0);
        op.shapeTopSide = mcpArgDbl(args, "topSide", 0.0);
    }
    brushTool_->applyMask(mask, op);
    return mcpOk();
}

// ---------------------------------------------------------------------------
// Terrain / palette
// ---------------------------------------------------------------------------

QVariantMap MapEditor::McpBridge::mcpHandleTerrain(const QString& name,
                                         const QVariantMap& args)
{
    static const QSet<QString> kTools = {
        QStringLiteral("get_terrain_at"), QStringLiteral("get_palette"),
        QStringLiteral("set_palette_color"),
    };
    if (!kTools.contains(name)) {
        return mcpUnhandled();
    }
    using MapEditor::EditorSession;
    const QString worldError = mcpRequireWorld();
    if (!worldError.isEmpty()) {
        return mcpError(worldError);
    }

    if (name == QLatin1String("get_terrain_at")) {
        const int x = mcpArgInt(args, "x", -1);
        const int y = mcpArgInt(args, "y", -1);
        EditorSession::TerrainSample sample;
        if (!session_->terrainSample(x, y, sample)) {
            return mcpError(QStringLiteral(
                "position (%1, %2) is outside the map or no world is loaded")
                                .arg(x).arg(y));
        }
        const auto rgbMap = [](unsigned rgb) {
            return QVariantMap{{QStringLiteral("r"), static_cast<int>((rgb >> 16) & 0xFF)},
                               {QStringLiteral("g"), static_cast<int>((rgb >> 8) & 0xFF)},
                               {QStringLiteral("b"), static_cast<int>(rgb & 0xFF)}};
        };
        return mcpOk({
            {QStringLiteral("geoHeight"), sample.geoHeight},
            {QStringLiteral("damHeight"), sample.damHeight},
            {QStringLiteral("surfaceIndex"), sample.surfaceIndex},
            {QStringLiteral("attributes"), static_cast<int>(sample.attributes)},
            {QStringLiteral("geoColor"), rgbMap(sample.geoRgb)},
            {QStringLiteral("damColor"), rgbMap(sample.damRgb)},
        });
    }

    if (name == QLatin1String("get_palette")) {
        const QString palette = mcpArgStr(args, "palette");
        const EditorSession::PaletteLayer layer =
            palette == QLatin1String("geo") ? EditorSession::PaletteLayer::Geo
                                            : EditorSession::PaletteLayer::Dam;
        QVariantList colors;
        for (int i = 0; i < 256; ++i) {
            const unsigned rgb = session_->paletteRgb(layer, i);
            colors.append(QVariantMap{
                {QStringLiteral("index"), i},
                {QStringLiteral("r"), static_cast<int>((rgb >> 16) & 0xFF)},
                {QStringLiteral("g"), static_cast<int>((rgb >> 8) & 0xFF)},
                {QStringLiteral("b"), static_cast<int>(rgb & 0xFF)},
            });
        }
        return mcpOk({{QStringLiteral("colors"), colors}});
    }

    // set_palette_color
    const QString palette = mcpArgStr(args, "palette");
    const int index = mcpArgInt(args, "index", -1);
    if (index < 0 || index > 255) {
        return mcpError(QStringLiteral("palette index must be 0..255"));
    }
    const EditorSession::PaletteLayer layer =
        palette == QLatin1String("geo") ? EditorSession::PaletteLayer::Geo
                                        : EditorSession::PaletteLayer::Dam;
    const unsigned rgb =
        (static_cast<unsigned>(std::clamp(mcpArgInt(args, "r"), 0, 255)) << 16) |
        (static_cast<unsigned>(std::clamp(mcpArgInt(args, "g"), 0, 255)) << 8) |
        static_cast<unsigned>(std::clamp(mcpArgInt(args, "b"), 0, 255));
    if (!session_->setPaletteRgb(layer, index, rgb)) {
        return mcpError(QStringLiteral("setPaletteRgb failed"));
    }
    session_->applyPalettes();
    paletteRefreshHook_();
    return mcpOk();
}

// ---------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------

QVariantMap MapEditor::McpBridge::mcpHandleHistory(const QString& name,
                                         const QVariantMap& args)
{
    static const QSet<QString> kTools = {
        QStringLiteral("undo"), QStringLiteral("redo"),
    };
    if (!kTools.contains(name)) {
        return mcpUnhandled();
    }
    const QString worldError = mcpRequireWorld();
    if (!worldError.isEmpty()) {
        return mcpError(worldError);
    }
    if (name == QLatin1String("undo")) {
        return QVariantMap{
            {QStringLiteral("ok"), session_->undo()},
            {QStringLiteral("note"),
             QStringLiteral("consecutive edits within 500 ms form one undo "
                            "transaction")},
        };
    }
    return QVariantMap{{QStringLiteral("ok"), session_->redo()}};
}

MapEditor::McpBridge::McpBridge(MapEditor::EditorSession *session,
                                 MapEditor::ViewportWidget *viewport,
                                 MapEditor::BrushTool *brushTool,
                                 MapEditor::BattleMapController *battleMaps,
                                 MapEditor::BrushUiController *brushUI,
                                 QObject *parent)
    : QObject(parent),
      session_(session),
      viewport_(viewport),
      brushTool_(brushTool),
      battleMaps_(battleMaps),
      brushUI_(brushUI)
{
}

MapEditor::McpBridge::~McpBridge()
{
    delete mcpServer_; // stops serving and releases the port
    mcpServer_ = nullptr;
}

void MapEditor::McpBridge::setWorldUnloadHook(std::function<void()> hook)
{
    worldUnloadHook_ = std::move(hook);
}

void MapEditor::McpBridge::setWorldLoadedHook(std::function<void()> hook)
{
    worldLoadedHook_ = std::move(hook);
}

void MapEditor::McpBridge::setStatusBarHook(std::function<void()> hook)
{
    statusBarHook_ = std::move(hook);
}

void MapEditor::McpBridge::setPaletteRefreshHook(std::function<void()> hook)
{
    paletteRefreshHook_ = std::move(hook);
}

#endif // MAP_EDITOR_MCP
