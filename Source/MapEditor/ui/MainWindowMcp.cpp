// ============================================================================
// MainWindow — MCP server bridge (MainWindowMcp.cpp).
//
// Wires the embedded MCP server (mcp/McpServer) to the editor: one handler
// per catalog tool, calling the same core paths as the UI actions and
// panels — dialog flows are replaced by plain string/number arguments.
// The server runs on the UI thread, so handlers touch vMap/editor state
// exactly like the widgets do.
//
// Port configuration: map_editor.ini next to the executable,
//   [mcp]
//   port=10102
// ============================================================================

#ifdef MAP_EDITOR_MCP

#include "MainWindow.h"

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

void MainWindow::setupMcpServer()
{
    mcpServer_ = new MapEditor::McpServer(this);
    mcpServer_->setTools(MapEditor::mcpToolCatalog());
    mcpServer_->setToolHandler(
        [this](const QString& name, const QVariantMap& args) {
            return mcpInvoke(name, args);
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
        return;
    }
    MapEditor::editorTrace("MCP server listening on port %u (config: %s)",
                mcpServer_->port(), iniPath.toUtf8().constData());
}

QVariantMap MainWindow::mcpInvoke(const QString& name, const QVariantMap& args)
{
    using MapEditor::EditorSession;

    const auto requireGameDir = [&]() -> QString {
        if (!session_ || !session_->hasGameDir()) {
            return QStringLiteral("set the game path first (set_game_path)");
        }
        return QString();
    };
    const auto requireWorld = [&]() -> QString {
        const QString dirError = requireGameDir();
        if (!dirError.isEmpty()) {
            return dirError;
        }
        if (!session_->isWorldLoaded()) {
            return QStringLiteral("no world loaded (open_world first)");
        }
        return QString();
    };
    const auto requireBattleMap = [&]() -> QString {
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return worldError;
        }
        if (!battleMap_.loaded()) {
            return QStringLiteral("no battle map loaded (open_battle_map first)");
        }
        return QString();
    };
    const auto argInt = [&args](const char* key, int fallback = 0) {
        return args.value(QLatin1String(key), fallback).toInt();
    };
    const auto argDbl = [&args](const char* key, double fallback = 0.0) {
        return args.value(QLatin1String(key), fallback).toDouble();
    };
    const auto argStr = [&args](const char* key) {
        return args.value(QLatin1String(key)).toString();
    };

    // ------------------------------------------------------------------
    // Session / world
    // ------------------------------------------------------------------
    if (name == QLatin1String("set_game_path")) {
        const QString path = argStr("path");
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
        onWorldUnload();
        if (!session_->scanWorlds()) {
            return mcpError(QStringLiteral(
                "game directory set, but no worlds found in RESOURCE/Worlds"));
        }
        if (battleMapPanel_) {
            battleMapPanel_->setTemplates(battleMapTemplates());
        }
        updateStatusBar();
        return mcpOk({{QStringLiteral("worlds"),
                       static_cast<int>(session_->getWorldList().size())}});
    }

    if (name == QLatin1String("list_worlds")) {
        const QString dirError = requireGameDir();
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
        const QString worldError = requireWorld();
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
        const QString dirError = requireGameDir();
        if (!dirError.isEmpty()) {
            return mcpError(dirError);
        }
        const std::string worldName = argStr("name").toStdString();
        if (!MapEditor::isValidWorldName(worldName)) {
            return mcpError(QStringLiteral("invalid world name '%1'")
                                .arg(argStr("name")));
        }
        MapEditor::NewWorldParams params;
        params.name = worldName;
        params.mapPowerX = params.mapPowerY = argInt("mapPower", 11);
        params.hZeroPlast = argInt("hZeroPlast", 30);
        params.geoLayerHeight = argInt("geoLayerHeight", 20);
        params.flatSurface = args.value(QStringLiteral("flat"), true).toBool();
        const std::string templ = argStr("templateWorld").toStdString();
        const MapEditor::WorldCreateResult result =
            MapEditor::createNewWorld(*session_, params, templ);
        if (!result.ok) {
            return mcpError(QStringLiteral("createNewWorld failed: %1")
                                .arg(QString::fromStdString(result.error)));
        }
        onWorldLoaded();
        updateStatusBar();
        return mcpOk();
    }

    if (name == QLatin1String("open_world")) {
        const QString dirError = requireGameDir();
        if (!dirError.isEmpty()) {
            return mcpError(dirError);
        }
        const std::string worldName = argStr("name").toStdString();
        if (!session_->loadWorldByName(worldName)) {
            return mcpError(QStringLiteral("failed to load world '%1'")
                                .arg(argStr("name")));
        }
        onWorldLoaded();
        updateStatusBar();
        return mcpOk();
    }

    if (name == QLatin1String("save_world")) {
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        if (!session_->saveWorld()) {
            return mcpError(QStringLiteral("saveWorld failed"));
        }
        updateStatusBar();
        return mcpOk();
    }

    // ------------------------------------------------------------------
    // Battle maps
    // ------------------------------------------------------------------
    if (name == QLatin1String("list_battle_maps")) {
        const QString dirError = requireGameDir();
        if (!dirError.isEmpty()) {
            return mcpError(dirError);
        }
        return mcpOk({{QStringLiteral("battleMaps"), battleMapTemplates()}});
    }

    if (name == QLatin1String("open_battle_map")) {
        const QString dirError = requireGameDir();
        if (!dirError.isEmpty()) {
            return mcpError(dirError);
        }
        QString fileName = argStr("name");
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
        if (!battleMap_.load(path.toStdString())) {
            return mcpError(QStringLiteral("cannot read the battle map file"));
        }
        battleMapPath_ = path;
        checkBattleMapNameMatchesWorld(path, battleMap_.worldName());
        ensureWorldForBattleMap();
        refreshBattleMapUI();
        return mcpOk({{QStringLiteral("world"),
                       QString::fromStdString(battleMap_.worldName())}});
    }

    if (name == QLatin1String("save_battle_map")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        if (battleMapPath_.isEmpty()) {
            return mcpError(QStringLiteral(
                "the battle map has no file path; use new_battle_map"));
        }
        normalizeBattleMapSpawnHeights();
        syncScumTriggerArtifacts();
        if (!battleMap_.save(battleMapPath_.toStdString())) {
            return mcpError(QStringLiteral("failed to write the battle map"));
        }
        flushScumTriggerFile();
        return mcpOk({{QStringLiteral("path"), battleMapPath_}});
    }

    if (name == QLatin1String("new_battle_map")) {
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        const QString mapName = argStr("name");
        const QString templ = argStr("template").isEmpty()
                                  ? battleMapTemplates().value(0)
                                  : argStr("template");
        if (mapName.isEmpty() || templ.isEmpty()) {
            return mcpError(QStringLiteral("name and template are required"));
        }
        onNewBattleMapRequested(mapName, templ, argInt("players", 2));
        if (!battleMap_.loaded()) {
            return mcpError(QStringLiteral("new_battle_map failed"));
        }
        return mcpOk();
    }

    // ------------------------------------------------------------------
    // Difference mode (.gmp)
    // ------------------------------------------------------------------
    if (name == QLatin1String("set_difference_mode")) {
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        const bool enabled = args.value(QStringLiteral("enabled")).toBool();
        if (!session_->setDiffMode(enabled)) {
            return mcpError(QStringLiteral("setDiffMode(%1) failed")
                                .arg(enabled ? 1 : 0));
        }
        refreshPaletteRenderers();
        return mcpOk({{QStringLiteral("diffMode"), session_->isDiffMode()},
                      {QStringLiteral("hasDiffData"), session_->hasDiffData()},
                      {QStringLiteral("unsavedDiffChanges"),
                       session_->hasUnsavedDiffChanges()},
                      {QStringLiteral("note"),
                       QStringLiteral("switching cleared the undo history")}});
    }

    if (name == QLatin1String("save_difference") ||
        name == QLatin1String("load_difference")) {
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        const bool isSave = name == QLatin1String("save_difference");
        QString path = argStr("file");
        if (path.isEmpty() && !battleMapPath_.isEmpty()) {
            // The game looks for <map>.gmp next to <map>.spg.
            path = battleMapPath_;
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
            refreshPaletteRenderers();
        }
        return mcpOk({{QStringLiteral("path"), path},
                      {QStringLiteral("hasDiffData"), session_->hasDiffData()},
                      {QStringLiteral("diffMode"), session_->isDiffMode()}});
    }

    // ------------------------------------------------------------------
    // Battle map players
    // ------------------------------------------------------------------
    if (name == QLatin1String("list_players")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        QVariantList list;
        const int count = battleMap_.playerCount();
        for (int i = 0; i < count; ++i) {
            const MapEditor::SpgFile::PlayerData data = battleMap_.playerData(i);
            double ax = 0;
            double ay = 0;
            battleMap_.playerAnchor(i, ax, ay);
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

    if (name == QLatin1String("update_player")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        const int index = argInt("index", -1);
        if (index < 0 || index >= battleMap_.playerCount()) {
            return mcpError(QStringLiteral("no player at index %1").arg(index));
        }
        MapEditor::SpgFile::PlayerData data = battleMap_.playerData(index);
        if (args.contains(QStringLiteral("name"))) {
            data.name = argStr("name").toStdString();
        }
        if (args.contains(QStringLiteral("realPlayerType"))) {
            data.realPlayerType = argStr("realPlayerType").toStdString();
        }
        if (args.contains(QStringLiteral("belligerent"))) {
            data.belligerent = argStr("belligerent").toStdString();
        }
        if (args.contains(QStringLiteral("colorIndex"))) {
            data.colorIndex = argInt("colorIndex");
        }
        if (args.contains(QStringLiteral("clan"))) {
            data.clan = argInt("clan");
        }
        if (args.contains(QStringLiteral("difficulty"))) {
            data.difficulty = argStr("difficulty").toStdString();
        }
        if (args.contains(QStringLiteral("handicap"))) {
            data.handicap = argInt("handicap");
        }
        if (!battleMap_.setPlayerData(index, data)) {
            return mcpError(QStringLiteral("setPlayerData failed"));
        }
        if (args.contains(QStringLiteral("x")) ||
            args.contains(QStringLiteral("y"))) {
            double oldX = 0;
            double oldY = 0;
            battleMap_.playerAnchor(index, oldX, oldY);
            const double newX = argDbl("x", oldX);
            const double newY = argDbl("y", oldY);
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
                !battleMap_.setPlayerAnchor(index, newX, newY, dz)) {
                return mcpError(QStringLiteral(
                    "failed to move the spawn group - the location was not "
                    "changed"));
            }
            syncSpawnOverlay();
        }
        if (battleMapPanel_) {
            battleMapPanel_->refresh();
        }
        return mcpOk();
    }

    if (name == QLatin1String("add_player")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        const int count = battleMap_.playerCount();
        if (count >= 8) {
            return mcpError(QStringLiteral(
                "a battle map holds at most 8 players"));
        }
        // Grow by cloning the last player (the setPlayerCount grow path).
        if (!battleMap_.setPlayerCount(count + 1)) {
            return mcpError(QStringLiteral("setPlayerCount failed"));
        }
        const int index = count; // the new last player
        // Apply any provided properties to the clone.
        MapEditor::SpgFile::PlayerData data = battleMap_.playerData(index);
        if (args.contains(QStringLiteral("name"))) {
            data.name = argStr("name").toStdString();
        }
        if (args.contains(QStringLiteral("realPlayerType"))) {
            data.realPlayerType = argStr("realPlayerType").toStdString();
        }
        if (args.contains(QStringLiteral("belligerent"))) {
            data.belligerent = argStr("belligerent").toStdString();
        }
        if (args.contains(QStringLiteral("colorIndex"))) {
            data.colorIndex = argInt("colorIndex");
        }
        if (args.contains(QStringLiteral("clan"))) {
            data.clan = argInt("clan");
        }
        if (args.contains(QStringLiteral("difficulty"))) {
            data.difficulty = argStr("difficulty").toStdString();
        }
        if (args.contains(QStringLiteral("handicap"))) {
            data.handicap = argInt("handicap");
        }
        if (!battleMap_.setPlayerData(index, data)) {
            return mcpError(QStringLiteral("setPlayerData failed"));
        }
        // Optional spawn placement: terrain-corrected like update_player.
        if (args.contains(QStringLiteral("x")) ||
            args.contains(QStringLiteral("y"))) {
            double oldX = 0;
            double oldY = 0;
            battleMap_.playerAnchor(index, oldX, oldY);
            const double newX = argDbl("x", oldX);
            const double newY = argDbl("y", oldY);
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
                !battleMap_.setPlayerAnchor(index, newX, newY, dz)) {
                return mcpError(QStringLiteral(
                    "failed to place the new spawn group"));
            }
        }
        syncSpawnOverlay();
        if (battleMapPanel_) {
            battleMapPanel_->refresh();
        }
        return mcpOk({{QStringLiteral("index"), index},
                      {QStringLiteral("players"), battleMap_.playerCount()}});
    }

    if (name == QLatin1String("remove_player")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        const int index = argInt("index", -1);
        if (index < 0 || index >= battleMap_.playerCount()) {
            return mcpError(QStringLiteral("no player at index %1").arg(index));
        }
        if (!battleMap_.removePlayer(index)) {
            return mcpError(QStringLiteral(
                "removePlayer failed (invalid index or the last player?)"));
        }
        refreshBattleMapUI();
        return mcpOk({{QStringLiteral("players"), battleMap_.playerCount()}});
    }

    // ------------------------------------------------------------------
    // Battle map objects
    // ------------------------------------------------------------------
    if (name == QLatin1String("list_battle_map_objects")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        QVariantList buildings;
        std::vector<MapEditor::SpgFile::PlacedBuilding> list = battleMap_.buildings();
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
        std::vector<MapEditor::SpgFile::ScumSpot> nests = battleMap_.scumSpots();
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
            battleMap_.natureObjects();
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
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        const int attributeId = argInt("attributeId", -1);
        const MapEditor::BuildingInfo* info = MapEditor::buildingInfo(attributeId);
        if (!info) {
            return mcpError(QStringLiteral("attributeId must be 0..42, got %1")
                                .arg(attributeId));
        }
        const double x = argDbl("x");
        const double y = argDbl("y");
        if (buildingPlacementBlocked(x, y, info->radius)) {
            return mcpError(QStringLiteral(
                "the placement intersects another building"));
        }
        MapEditor::SpgFile::PlacedBuilding b;
        b.unitId = battleMap_.maxUnitId() + 1;
        b.attributeId = attributeId;
        b.x = x;
        b.y = y;
        b.z = session_->zeroLayerHeight() + info->zOffset;
        b.angleDeg = argDbl("angle", 0.0);
        b.hp = argInt("hp", 100);
        if (!battleMap_.addBuilding(b)) {
            return mcpError(QStringLiteral("addBuilding failed"));
        }
        reloadPlacedBuildings();
        return mcpOk({{QStringLiteral("unitId"), b.unitId}});
    }

    if (name == QLatin1String("update_building")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        reloadPlacedBuildings();
        const int row = argInt("index", -1);
        if (row < 0 || row >= static_cast<int>(placedBuildings_.size())) {
            return mcpError(QStringLiteral("no building at index %1").arg(row));
        }
        const MapEditor::SpgFile::PlacedBuilding& b =
            placedBuildings_[static_cast<std::size_t>(row)];
        const bool moved = args.contains(QStringLiteral("x")) ||
                           args.contains(QStringLiteral("y"));
        if (!battleMap_.setBuildingPose(
                b.index, argDbl("x", b.x), argDbl("y", b.y),
                argDbl("angle", b.angleDeg))) {
            return mcpError(QStringLiteral("setBuildingPose failed"));
        }
        if (args.contains(QStringLiteral("hp")) &&
            !battleMap_.setBuildingHp(b.index, argInt("hp"))) {
            return mcpError(QStringLiteral("setBuildingHp failed"));
        }
        Q_UNUSED(moved);
        reloadPlacedBuildings();
        return mcpOk();
    }

    if (name == QLatin1String("delete_building")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        reloadPlacedBuildings();
        const int row = argInt("index", -1);
        if (row < 0 || row >= static_cast<int>(placedBuildings_.size())) {
            return mcpError(QStringLiteral("no building at index %1").arg(row));
        }
        if (!battleMap_.removeBuilding(placedBuildings_[static_cast<std::size_t>(row)].index)) {
            return mcpError(QStringLiteral("removeBuilding failed"));
        }
        reloadPlacedBuildings();
        return mcpOk();
    }

    if (name == QLatin1String("place_scum_spot")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        const int attributeId = argInt("attributeId", -1);
        const MapEditor::ScumSpotInfo* info = MapEditor::scumSpotInfo(attributeId);
        if (!info) {
            return mcpError(QStringLiteral("attributeId must be 43..46, got %1")
                                .arg(attributeId));
        }
        const double x = argDbl("x");
        const double y = argDbl("y");
        for (const MapEditor::SpgFile::ScumSpot& s : battleMap_.scumSpots()) {
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
        spot.angleDeg = argDbl("angle", 0.0);
        spot.hp = 100;
        spot.filth.filthType = argInt("filthType", 2);
        spot.filth.creatureNum = argInt("creatureNum", 10);
        if (!battleMap_.addScumSpot(spot)) {
            return mcpError(QStringLiteral("addScumSpot failed"));
        }
        reloadScumSpots();
        return mcpOk({{QStringLiteral("label"), QString::fromStdString(spot.label)}});
    }

    if (name == QLatin1String("update_scum_spot")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        const std::vector<MapEditor::SpgFile::ScumSpot> nests = battleMap_.scumSpots();
        const int row = argInt("index", -1);
        if (row < 0 || row >= static_cast<int>(nests.size())) {
            return mcpError(QStringLiteral("no scum spot at index %1").arg(row));
        }
        const MapEditor::SpgFile::ScumSpot& s = nests[static_cast<std::size_t>(row)];
        if (args.contains(QStringLiteral("x")) ||
            args.contains(QStringLiteral("y")) ||
            args.contains(QStringLiteral("angle"))) {
            if (!battleMap_.setScumSpotPose(
                    s.index, argDbl("x", s.x), argDbl("y", s.y),
                    argDbl("angle", s.angleDeg))) {
                return mcpError(QStringLiteral("setScumSpotPose failed"));
            }
        }
        MapEditor::SpgFile::FilthParams filth = s.filth;
        if (args.contains(QStringLiteral("filthType"))) {
            filth.filthType = argInt("filthType");
        }
        if (args.contains(QStringLiteral("creatureNum"))) {
            filth.creatureNum = argInt("creatureNum");
        }
        if (args.contains(QStringLiteral("sleep"))) {
            filth.sleep = args.value(QStringLiteral("sleep")).toBool();
        }
        if (args.contains(QStringLiteral("sleepPeriod"))) {
            filth.sleepPeriod = static_cast<float>(argDbl("sleepPeriod"));
        }
        if (args.contains(QStringLiteral("attackPeriod"))) {
            filth.attackPeriond = static_cast<float>(argDbl("attackPeriod"));
        }
        if (args.contains(QStringLiteral("activatingUnit"))) {
            filth.activatingUnit = argInt("activatingUnit");
        }
        if (args.contains(QStringLiteral("activatingDistance"))) {
            filth.activatingDistance = static_cast<float>(argDbl("activatingDistance"));
        }
        if (args.contains(QStringLiteral("attackPlayer"))) {
            filth.attackPlayer = argInt("attackPlayer");
        }
        if (!battleMap_.setScumSpotFilth(s.index, filth)) {
            return mcpError(QStringLiteral("setScumSpotFilth failed"));
        }
        reloadScumSpots();
        return mcpOk();
    }

    if (name == QLatin1String("delete_scum_spot")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        const std::vector<MapEditor::SpgFile::ScumSpot> nests = battleMap_.scumSpots();
        const int row = argInt("index", -1);
        if (row < 0 || row >= static_cast<int>(nests.size())) {
            return mcpError(QStringLiteral("no scum spot at index %1").arg(row));
        }
        if (!battleMap_.removeScumSpot(nests[static_cast<std::size_t>(row)].index)) {
            return mcpError(QStringLiteral("removeScumSpot failed"));
        }
        reloadScumSpots();
        return mcpOk();
    }

    if (name == QLatin1String("place_nature_object")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        const QString model = argStr("model");
        if (model.isEmpty()) {
            return mcpError(QStringLiteral("model must not be empty"));
        }
        MapEditor::SpgFile::NatureObject object;
        object.modelName = model.toStdString();
        object.x = argDbl("x");
        object.y = argDbl("y");
        object.z = args.contains(QStringLiteral("z"))
                       ? argDbl("z")
                       : session_->zeroLayerHeight();
        object.angleDeg = argDbl("angle", 0.0);
        object.radius = argDbl("radius", 10.0);
        object.destroyable = args.value(QStringLiteral("destroy"), true).toBool();
        object.collisions = args.value(QStringLiteral("collisions"), true).toBool();
        object.lighting = args.value(QStringLiteral("lighting"), false).toBool();
        if (!battleMap_.addNatureObject(object)) {
            return mcpError(QStringLiteral("addNatureObject failed"));
        }
        reloadNatureObjects();
        return mcpOk({{QStringLiteral("unitId"), object.unitId}});
    }

    if (name == QLatin1String("update_nature_object")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        const std::vector<MapEditor::SpgFile::NatureObject> objects =
            battleMap_.natureObjects();
        const int row = argInt("index", -1);
        if (row < 0 || row >= static_cast<int>(objects.size())) {
            return mcpError(QStringLiteral("no nature object at index %1").arg(row));
        }
        const MapEditor::SpgFile::NatureObject& n = objects[static_cast<std::size_t>(row)];
        if (args.contains(QStringLiteral("x")) ||
            args.contains(QStringLiteral("y")) ||
            args.contains(QStringLiteral("z")) ||
            args.contains(QStringLiteral("angle"))) {
            if (!battleMap_.setNatureObjectPose(
                    n.index, argDbl("x", n.x), argDbl("y", n.y),
                    argDbl("z", n.z), argDbl("angle", n.angleDeg))) {
                return mcpError(QStringLiteral("setNatureObjectPose failed"));
            }
        }
        if (args.contains(QStringLiteral("radius")) ||
            args.contains(QStringLiteral("destroy")) ||
            args.contains(QStringLiteral("collisions")) ||
            args.contains(QStringLiteral("lighting"))) {
            if (!battleMap_.setNatureObjectShape(
                    n.index, argDbl("radius", n.radius),
                    args.value(QStringLiteral("destroy"), n.destroyable).toBool(),
                    args.value(QStringLiteral("collisions"), n.collisions).toBool(),
                    args.value(QStringLiteral("lighting"), n.lighting).toBool())) {
                return mcpError(QStringLiteral("setNatureObjectShape failed"));
            }
        }
        reloadNatureObjects();
        return mcpOk();
    }

    if (name == QLatin1String("delete_nature_object")) {
        const QString bmError = requireBattleMap();
        if (!bmError.isEmpty()) {
            return mcpError(bmError);
        }
        const std::vector<MapEditor::SpgFile::NatureObject> objects =
            battleMap_.natureObjects();
        const int row = argInt("index", -1);
        if (row < 0 || row >= static_cast<int>(objects.size())) {
            return mcpError(QStringLiteral("no nature object at index %1").arg(row));
        }
        if (!battleMap_.removeNatureObject(objects[static_cast<std::size_t>(row)].index)) {
            return mcpError(QStringLiteral("removeNatureObject failed"));
        }
        reloadNatureObjects();
        return mcpOk();
    }

    // ------------------------------------------------------------------
    // Brushes
    // ------------------------------------------------------------------
    if (name == QLatin1String("load_decal_image")) {
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        const QString filePath = argStr("file");
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
        refreshPaletteRenderers();
        brushTool_->setDecalImage(prepared.image);
        return mcpOk({{QStringLiteral("width"), width},
                      {QStringLiteral("height"), height},
                      {QStringLiteral("paletteWrites"),
                       static_cast<int>(prepared.paletteWrites.size())}});
    }

    if (name == QLatin1String("import_model")) {
        const QString filePath = argStr("file");
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
        const bool quantized = quantizeModelBrush();
        return mcpOk({{QStringLiteral("width"), imported.model.width},
                      {QStringLiteral("height"), imported.model.height},
                      {QStringLiteral("quantized"), quantized}});
    }

    if (name == QLatin1String("apply_brush")) {
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        const QString effectName = argStr("effect");
        const int x = argInt("x");
        const int y = argInt("y");
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
                op.modelMode = std::clamp(argInt("mode", 0), 0, 2);
                op.modelHeight = argInt("modelHeight", 32);
                op.modelScaleX = std::clamp(argDbl("scaleX", 1.0), 0.0, 1.0);
                op.modelScaleY = std::clamp(argDbl("scaleY", 1.0), 0.0, 1.0);
            }
            brushTool_->setParams(op);
            if (!brushTool_->applyStamp(x, y)) {
                return mcpError(QStringLiteral(
                    "the stamp was refused (Dam layer only; is the image/"
                    "model loaded?)"));
            }
            return mcpOk();
        }
        int radius = argInt("radius", 1);
        if (radius < 1) {
            return mcpError(QStringLiteral("radius must be >= 1"));
        }
        radius = MapEditor::BrushTool::clampRadius(radius);
        const bool square = args.value(QStringLiteral("square"), false).toBool();

        MapEditor::BrushMask mask;
        const int sides = argInt("sides", 0);
        if (sides >= MapEditor::kMinPolygonSides) {
            mask = MapEditor::BrushMasks::filledPolygon(
                radius, sides, argDbl("angleOffset", 0.0));
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
        const QString layer = argStr("layer").isEmpty()
                                  ? QStringLiteral("dam")
                                  : argStr("layer");
        op.mode = layer == QLatin1String("geo") ? MapEditor::BrushMode::Geo
                                                : MapEditor::BrushMode::Dam;
        const auto setSculpt = [&]() {
            op.mode = MapEditor::BrushMode::Dam; // sculpting owns the dam layer
            op.sculptEffect = MapEditor::BrushTool::clampSculptEffect(
                argDbl("fraction", 1.0));
        };
        const bool geoRequested = op.mode == MapEditor::BrushMode::Geo;

        if (effectName == QLatin1String("raiseHeight")) {
            op.effect = MapEditor::BrushEffect::ChangeHeight;
            op.direction = 1;
            op.strength = MapEditor::BrushTool::clampStrength(argInt("height", 1));
        } else if (effectName == QLatin1String("lowerHeight")) {
            op.effect = MapEditor::BrushEffect::ChangeHeight;
            op.direction = -1;
            op.strength = MapEditor::BrushTool::clampStrength(argInt("height", 1));
        } else if (effectName == QLatin1String("setHeight")) {
            op.effect = MapEditor::BrushEffect::AlignVariableH;
            op.heightLevel = MapEditor::BrushTool::clampHeightLevel(argInt("height", 0));
        } else if (effectName == QLatin1String("setToZP")) {
            op.effect = MapEditor::BrushEffect::Align4ZP;
        } else if (effectName == QLatin1String("paintSoot")) {
            op.effect = MapEditor::BrushEffect::PlotSoot;
        } else if (effectName == QLatin1String("paintVeryLightDam")) {
            op.effect = MapEditor::BrushEffect::VeryLightDam;
        } else if (effectName == QLatin1String("paintColor")) {
            op.effect = MapEditor::BrushEffect::SetColor;
            int colorIndex = argInt("colorIndex", -1);
            if (colorIndex < 0 && args.contains(QStringLiteral("colorR"))) {
                // Nearest entry of the layer palette, like the JS paintColor.
                const int r = argInt("colorR");
                const int g = argInt("colorG");
                const int b = argInt("colorB");
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
                argInt("blurRadius", 2), radius);
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
        const QString shapeName = argStr("shape");
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
            op.shapeHeight = argInt("shapeHeight", 32);
            op.shapeTopOffsetX = argDbl("topOffsetX", 0.0);
            op.shapeTopOffsetY = argDbl("topOffsetY", 0.0);
            op.shapeTopRadius = argDbl("topRadius", 0.0);
            op.shapeTopSide = argDbl("topSide", 0.0);
        }
        brushTool_->applyMask(mask, op);
        return mcpOk();
    }

    // ------------------------------------------------------------------
    // Scripting
    // ------------------------------------------------------------------
    if (name == QLatin1String("run_script")) {
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        const std::string script = argStr("script").toStdString();
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
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        MapEditor::BrushParams params = brushTool_->params();
        params.script = argStr("script").toStdString();
        brushTool_->setParams(params);
        return mcpOk();
    }

    // ------------------------------------------------------------------
    // Terrain / palette
    // ------------------------------------------------------------------
    if (name == QLatin1String("get_terrain_at")) {
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        const int x = argInt("x", -1);
        const int y = argInt("y", -1);
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
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        const QString palette = argStr("palette");
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

    if (name == QLatin1String("set_palette_color")) {
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        const QString palette = argStr("palette");
        const int index = argInt("index", -1);
        if (index < 0 || index > 255) {
            return mcpError(QStringLiteral("palette index must be 0..255"));
        }
        const EditorSession::PaletteLayer layer =
            palette == QLatin1String("geo") ? EditorSession::PaletteLayer::Geo
                                            : EditorSession::PaletteLayer::Dam;
        const unsigned rgb =
            (static_cast<unsigned>(std::clamp(argInt("r"), 0, 255)) << 16) |
            (static_cast<unsigned>(std::clamp(argInt("g"), 0, 255)) << 8) |
            static_cast<unsigned>(std::clamp(argInt("b"), 0, 255));
        if (!session_->setPaletteRgb(layer, index, rgb)) {
            return mcpError(QStringLiteral("setPaletteRgb failed"));
        }
        session_->applyPalettes();
        refreshPaletteRenderers();
        return mcpOk();
    }

    // ------------------------------------------------------------------
    // Undo / redo
    // ------------------------------------------------------------------
    if (name == QLatin1String("undo")) {
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        return QVariantMap{
            {QStringLiteral("ok"), session_->undo()},
            {QStringLiteral("note"),
             QStringLiteral("consecutive edits within 500 ms form one undo "
                            "transaction")},
        };
    }
    if (name == QLatin1String("redo")) {
        const QString worldError = requireWorld();
        if (!worldError.isEmpty()) {
            return mcpError(worldError);
        }
        return QVariantMap{{QStringLiteral("ok"), session_->redo()}};
    }

    return mcpError(QStringLiteral("unhandled tool: %1").arg(name));
}

#endif // MAP_EDITOR_MCP
