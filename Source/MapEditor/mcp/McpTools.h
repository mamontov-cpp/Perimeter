#pragma once

// ============================================================================
// MapEditorMcp — the MCP tool catalog.
//
// One entry per tool the embedded MCP server exposes to LLM agents: name,
// human/agent-readable description, and a JSON Schema for the arguments.
// Shared by the editor bridge (ui/MainWindowMcp.cpp) and the tests, so the
// catalog is defined exactly once.
//
// All mutating tools return a small JSON object; failures come back as
// {"error": "..."} which the server turns into an MCP isError result.
// ============================================================================

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVector>

namespace MapEditor {

struct McpToolSpec
{
    QString name;
    QString description;
    QJsonObject inputSchema; ///< JSON Schema object for `arguments`.
};

/// Every tool the server offers, in a stable order.
inline QVector<McpToolSpec> mcpToolCatalog()
{
    const QString undoNote =
        QStringLiteral(" Undo: the editor groups consecutive edits applied "
                       "within 500 ms into a single undo transaction, so a "
                       "burst of rapid calls can be reverted with one undo.");
    auto obj = [](std::initializer_list<std::pair<QString, QJsonValue>> init) {
        QJsonObject o;
        for (const auto& [k, v] : init) {
            o.insert(k, v);
        }
        return o;
    };
    const QJsonArray str = QJsonArray{QStringLiteral("string")};
    const QJsonArray num = QJsonArray{QStringLiteral("number")};
    const QJsonArray integer = QJsonArray{QStringLiteral("integer")};
    const QJsonArray boolean = QJsonArray{QStringLiteral("boolean")};

    QVector<McpToolSpec> t;

    t.append({QStringLiteral("set_game_path"),
              QStringLiteral("Sets the Perimeter game directory — the folder "
                             "that contains RESOURCE/. Must be called before "
                             "worlds can be listed or loaded. Dialog-free "
                             "equivalent of the UI game-directory picker."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("path"),
                          obj({{QStringLiteral("type"), str}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("path")}}})});

    t.append({QStringLiteral("list_worlds"),
              QStringLiteral("Lists the worlds available for loading in the "
                             "selected game directory (base game plus mods, "
                             "as 'Mod / World' when applicable). Requires "
                             "set_game_path first. Returns name, dirKey, "
                             "modName and displayName per world."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), QJsonObject()}})});

    t.append({QStringLiteral("get_world_properties"),
              QStringLiteral("Properties of the currently loaded world: "
                             "loaded flag, name, map size in cells, and the "
                             "zero-layer height."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), QJsonObject()}})});

    t.append({QStringLiteral("create_world"),
              QStringLiteral("Creates a NEW world in RESOURCE/Worlds/<name> "
                             "from the parameters (dialog-free equivalent of "
                             "File - New World) and loads it. templateWorld "
                             "copies palettes/textures from an existing "
                             "world; flat=true makes a flat geo layer at "
                             "geoLayerHeight (must be below hZeroPlast), "
                             "flat=false generates procedural relief.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("name"),
                          obj({{QStringLiteral("type"), str}})},
                         {QStringLiteral("templateWorld"),
                          obj({{QStringLiteral("type"), str}})},
                         {QStringLiteral("mapPower"),
                          obj({{QStringLiteral("type"), integer},
                               {QStringLiteral("default"), 11}})},
                         {QStringLiteral("hZeroPlast"),
                          obj({{QStringLiteral("type"), integer},
                               {QStringLiteral("default"), 30}})},
                         {QStringLiteral("geoLayerHeight"),
                          obj({{QStringLiteral("type"), integer},
                               {QStringLiteral("default"), 20}})},
                         {QStringLiteral("flat"),
                          obj({{QStringLiteral("type"), boolean},
                               {QStringLiteral("default"), true}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("name")}}})});

    t.append({QStringLiteral("open_world"),
              QStringLiteral("Loads a world by its folder name (as listed by "
                             "list_worlds). The current world is unloaded "
                             "first; unsaved changes are discarded."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("name"),
                          obj({{QStringLiteral("type"), str}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("name")}}})});

    t.append({QStringLiteral("save_world"),
              QStringLiteral("Saves the currently loaded world to its folder. "
                             "World terrain data only — never merges battle "
                             "map difference (.gmp) data into the world."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), QJsonObject()}})});

    t.append({QStringLiteral("list_battle_maps"),
              QStringLiteral("Lists the .spg battle maps in the selected game "
                             "directory (RESOURCE/Battle and "
                             "RESOURCE/Worlds)."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), QJsonObject()}})});

    t.append({QStringLiteral("open_battle_map"),
              QStringLiteral("Opens a battle map (.spg) by file name (e.g. "
                             "'EXTDEMO.spg'); the world named inside the map "
                             "is loaded automatically. Activates the battle "
                             "map panels (players, buildings, scum nests, "
                             "nature)."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("name"),
                          obj({{QStringLiteral("type"), str}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("name")}}})});

    t.append({QStringLiteral("save_battle_map"),
              QStringLiteral("Saves the loaded battle map to its .spg file. "
                             "Normalizes the start poses of the player "
                             "frames and rewrites the scum-nest trigger "
                             "chain (Scripts/Triggers/<world>.scr) when "
                             "scum nests exist."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), QJsonObject()}})});

    t.append({QStringLiteral("list_battle_map_objects"),
              QStringLiteral("Lists the objects placed on the loaded battle "
                             "map: pre-placed buildings (attribute id/type, "
                             "position, rotation, HP), scum nests (mound + "
                             "filth spawner parameters) and static nature "
                             "objects (model, position, z, radius, flags). "
                             "Each entry carries its list index for use "
                             "with the update/delete tools."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), QJsonObject()}})});

    const QJsonObject indexProp = obj(
        {{QStringLiteral("index"),
          obj({{QStringLiteral("type"), integer},
               {QStringLiteral("description"),
                QStringLiteral("row from list_battle_map_objects")}})}});

    auto withIndex = [&](const QJsonObject& props) {
        QJsonObject merged = props;
        for (const QString& k : indexProp.keys()) {
            merged.insert(k, indexProp.value(k));
        }
        return merged;
    };

    t.append({QStringLiteral("place_building"),
              QStringLiteral("Places a pre-placed building on the loaded "
                             "battle map (into nobodysBuildings): attributeId "
                             "0..42 (0=Core, 1=Relay, 2=Collector, 3-5 "
                             "plants, 6=Command Center, 7-31 labs, 32-33 "
                             "corridors, 34 static bomb, 35-42 guns). The "
                             "building stands on the zero layer; placement "
                             "refuses intersections with other buildings.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    withIndex(obj({
                        {QStringLiteral("attributeId"),
                         obj({{QStringLiteral("type"), integer}})},
                        {QStringLiteral("x"), obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("y"), obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("angle"),
                         obj({{QStringLiteral("type"), num},
                              {QStringLiteral("default"), 0}})},
                        {QStringLiteral("hp"),
                         obj({{QStringLiteral("type"), integer},
                              {QStringLiteral("default"), 100}})}}))},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("attributeId"),
                               QStringLiteral("x"), QStringLiteral("y")}}})});
    // place_building takes no index; strip the helper artifact.
    t.last().inputSchema[QStringLiteral("properties")] =
        obj({{QStringLiteral("attributeId"),
              obj({{QStringLiteral("type"), integer}})},
             {QStringLiteral("x"), obj({{QStringLiteral("type"), num}})},
             {QStringLiteral("y"), obj({{QStringLiteral("type"), num}})},
             {QStringLiteral("angle"),
              obj({{QStringLiteral("type"), num},
                   {QStringLiteral("default"), 0}})},
             {QStringLiteral("hp"),
              obj({{QStringLiteral("type"), integer},
                   {QStringLiteral("default"), 100}})}});

    t.append({QStringLiteral("update_building"),
              QStringLiteral("Edits a placed building (from "
                             "list_battle_map_objects): position, rotation "
                             "and HP.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    withIndex(obj({
                        {QStringLiteral("x"), obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("y"), obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("angle"), obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("hp"),
                         obj({{QStringLiteral("type"), integer}})}}))},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("index")}}})});

    t.append({QStringLiteral("delete_building"),
              QStringLiteral("Deletes a placed building from the loaded "
                             "battle map (index from "
                             "list_battle_map_objects).") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), indexProp},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("index")}}})});

    t.append({QStringLiteral("place_scum_spot"),
              QStringLiteral("Places a scum nest: a SCUM_SPOT mound "
                             "(attributeId 43..46) on the zero layer plus an "
                             "invisible FILTH_SPOT spawner beneath it, wired "
                             "by a generated trigger chain. filthType is the "
                             "FILTH_SPOT_ID ordinal (1=ants, 2=wasp, "
                             "3=dragon, 7=crow, 11=volcano, ...). Nests must "
                             "be at least 150 units apart.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("attributeId"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("x"), obj({{QStringLiteral("type"), num}})},
                         {QStringLiteral("y"), obj({{QStringLiteral("type"), num}})},
                         {QStringLiteral("angle"),
                          obj({{QStringLiteral("type"), num},
                               {QStringLiteral("default"), 0}})},
                         {QStringLiteral("filthType"),
                          obj({{QStringLiteral("type"), integer},
                               {QStringLiteral("default"), 2}})},
                         {QStringLiteral("creatureNum"),
                          obj({{QStringLiteral("type"), integer},
                               {QStringLiteral("default"), 10}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("attributeId"),
                               QStringLiteral("x"), QStringLiteral("y")}}})});

    t.append({QStringLiteral("update_scum_spot"),
              QStringLiteral("Edits a scum nest (index from "
                             "list_battle_map_objects): position/rotation of "
                             "the pair, or the spawner parameters "
                             "(filthType, creatureNum, sleep, periods, "
                             "activatingUnit, activatingDistance, "
                             "attackPlayer).") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    withIndex(obj({
                        {QStringLiteral("x"), obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("y"), obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("angle"), obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("filthType"),
                         obj({{QStringLiteral("type"), integer}})},
                        {QStringLiteral("creatureNum"),
                         obj({{QStringLiteral("type"), integer}})},
                        {QStringLiteral("sleep"),
                         obj({{QStringLiteral("type"), boolean}})},
                        {QStringLiteral("sleepPeriod"),
                         obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("attackPeriod"),
                         obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("attackPlayer"),
                         obj({{QStringLiteral("type"), integer}})}}))},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("index")}}})});

    t.append({QStringLiteral("delete_scum_spot"),
              QStringLiteral("Deletes a scum nest (mound + spawner) from the "
                             "loaded battle map.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), indexProp},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("index")}}})});

    t.append({QStringLiteral("place_nature_object"),
              QStringLiteral("Places a static nature object (tree, rock, "
                             "effect) into SavePrm.environment. model is an "
                             "M3D path from list available under "
                             "RESOURCE/Models/Environment (e.g. "
                             "'RESOURCE\\Models\\Environment\\010_tree.M3D'). "
                             "z defaults to the zero layer height; radius "
                             "scales the model (its bound radius becomes "
                             "radius). Flags: destroy (can be destroyed), "
                             "collisions (real collisions), lighting.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("model"),
                          obj({{QStringLiteral("type"), str}})},
                         {QStringLiteral("x"), obj({{QStringLiteral("type"), num}})},
                         {QStringLiteral("y"), obj({{QStringLiteral("type"), num}})},
                         {QStringLiteral("z"), obj({{QStringLiteral("type"), num}})},
                         {QStringLiteral("angle"),
                          obj({{QStringLiteral("type"), num},
                               {QStringLiteral("default"), 0}})},
                         {QStringLiteral("radius"),
                          obj({{QStringLiteral("type"), num},
                               {QStringLiteral("default"), 10}})},
                         {QStringLiteral("destroy"),
                          obj({{QStringLiteral("type"), boolean},
                               {QStringLiteral("default"), true}})},
                         {QStringLiteral("collisions"),
                          obj({{QStringLiteral("type"), boolean},
                               {QStringLiteral("default"), true}})},
                         {QStringLiteral("lighting"),
                          obj({{QStringLiteral("type"), boolean},
                               {QStringLiteral("default"), false}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("model"), QStringLiteral("x"),
                               QStringLiteral("y")}}})});

    t.append({QStringLiteral("update_nature_object"),
              QStringLiteral("Edits a nature object (index from "
                             "list_battle_map_objects): position, z, "
                             "rotation, radius and the flags.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    withIndex(obj({
                        {QStringLiteral("x"), obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("y"), obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("z"), obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("angle"), obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("radius"), obj({{QStringLiteral("type"), num}})},
                        {QStringLiteral("destroy"),
                         obj({{QStringLiteral("type"), boolean}})},
                        {QStringLiteral("collisions"),
                         obj({{QStringLiteral("type"), boolean}})},
                        {QStringLiteral("lighting"),
                         obj({{QStringLiteral("type"), boolean}})}}))},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("index")}}})});

    t.append({QStringLiteral("delete_nature_object"),
              QStringLiteral("Deletes a nature object from the loaded battle "
                             "map.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), indexProp},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("index")}}})});

    QJsonObject shapeProp;
    shapeProp.insert(QStringLiteral("type"), str);
    shapeProp.insert(QStringLiteral("enum"),
                     QJsonArray{QStringLiteral("cone"), QStringLiteral("pyramid"),
                                QStringLiteral("truncatedCone"),
                                QStringLiteral("truncatedPyramid"),
                                QStringLiteral("torus"),
                                QStringLiteral("halfEllipsoid")});

    t.append({QStringLiteral("apply_brush"),
              QStringLiteral("Applies a terrain brush at (x, y) in map cells. "
                             "effect: raiseHeight, lowerHeight, setHeight, "
                             "setToZP (zero layer), paintSoot, "
                             "paintVeryLightDam, paintColor (needs colorR/"
                             "colorG/colorB or colorIndex), smooth (average), "
                             "flatten (fitted plane), tricubic (bicubic "
                             "lattice), inflate (normal offset), gaussianBlur "
                             "(needs blurRadius). layer: 'dam' (default, "
                             "includes all sculpting) or 'geo'. height in "
                             "whole voxels (1 voxel = 1/32 m) for the height "
                             "effects; fraction (0..1) is the sculpting blend "
                             "for smooth/flatten/tricubic/inflate; square "
                             "switches the stamp shape; sides/angleOffset "
                             "configure the regular-polygon mask. 3D SOLID "
                             "brushes: pass shape (cone, pyramid, "
                             "truncatedCone, truncatedPyramid, torus, "
                             "halfEllipsoid) together with a height effect "
                             "(raiseHeight, lowerHeight or setHeight): the "
                             "footprint is the shape's base at radius, and "
                             "each covered cell's target is height + "
                             "profile*shapeHeight; shapeHeight sets the "
                             "amplitude, topOffsetX/topOffsetY shift the "
                             "upper part, topRadius is the truncated "
                             "cone's top radius or the torus' inner radius, "
                             "topSide is the truncated pyramid's top edge "
                             "length, and sides/angleOffset define the "
                             "polygonal bases. 3D shapes are Dam-layer "
                             "only, like the sculpting effects. IMAGE DECAL: "
                             "effect 'imageDecal' stamps the picture loaded "
                             "by load_decal_image, centred at (x, y) "
                             "(radius is ignored). MODEL: effect 'model' "
                             "stamps the model imported by import_model as a "
                             "heightmap rectangle; modelHeight (voxels), "
                             "scaleX/scaleY (0..1) and mode (0 place, 1 add, "
                             "2 subtract) configure it. Both are Dam-layer "
                             "only. DIFFERENCE MODE: with set_difference_mode "
                             "ON every stamp writes the .gmp difference data "
                             "instead of the base terrain.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("effect"),
                          obj({{QStringLiteral("type"), str}})},
                         {QStringLiteral("x"), obj({{QStringLiteral("type"), num}})},
                         {QStringLiteral("y"), obj({{QStringLiteral("type"), num}})},
                         {QStringLiteral("radius"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("height"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("fraction"),
                          obj({{QStringLiteral("type"), num},
                               {QStringLiteral("default"), 1.0}})},
                         {QStringLiteral("blurRadius"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("square"),
                          obj({{QStringLiteral("type"), boolean},
                               {QStringLiteral("default"), false}})},
                         {QStringLiteral("shape"), shapeProp},
                         {QStringLiteral("shapeHeight"),
                          obj({{QStringLiteral("type"), integer},
                               {QStringLiteral("default"), 32}})},
                         {QStringLiteral("topOffsetX"),
                          obj({{QStringLiteral("type"), num},
                               {QStringLiteral("default"), 0}})},
                         {QStringLiteral("topOffsetY"),
                          obj({{QStringLiteral("type"), num},
                               {QStringLiteral("default"), 0}})},
                         {QStringLiteral("topRadius"),
                          obj({{QStringLiteral("type"), num},
                               {QStringLiteral("default"), 0}})},
                         {QStringLiteral("topSide"),
                          obj({{QStringLiteral("type"), num},
                               {QStringLiteral("default"), 0}})},
                         {QStringLiteral("modelHeight"),
                          obj({{QStringLiteral("type"), integer},
                               {QStringLiteral("default"), 32}})},
                         {QStringLiteral("scaleX"),
                          obj({{QStringLiteral("type"), num},
                               {QStringLiteral("default"), 1}})},
                         {QStringLiteral("scaleY"),
                          obj({{QStringLiteral("type"), num},
                               {QStringLiteral("default"), 1}})},
                         {QStringLiteral("mode"),
                          obj({{QStringLiteral("type"), integer},
                               {QStringLiteral("default"), 0}})},
                         {QStringLiteral("sides"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("angleOffset"),
                          obj({{QStringLiteral("type"), num}})},
                         {QStringLiteral("colorIndex"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("colorR"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("colorG"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("colorB"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("layer"),
                          obj({{QStringLiteral("type"), str},
                               {QStringLiteral("enum"),
                                QJsonArray{QStringLiteral("dam"),
                                           QStringLiteral("geo")}}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("effect"), QStringLiteral("x"),
                               QStringLiteral("y"), QStringLiteral("radius")}}})});

    t.append({QStringLiteral("run_script"),
              QStringLiteral("Runs a JavaScript through the editor's scripted "
                             "brush API (raiseHeight/setHeight/paintColor/"
                             "getDamHeight/applySmooth/... plus palette "
                             "setters). The script text replaces the Run "
                             "Script dialog's editor. The whole run is ONE "
                             "undo entry and honours the script time limit. "
                             "Errors report the script location."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("script"),
                          obj({{QStringLiteral("type"), str}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("script")}}})});

    t.append({QStringLiteral("set_scripted_brush_script"),
              QStringLiteral("Stores a JavaScript as the scripted-brush body "
                             "— the brush type that runs the script at "
                             "every stamp; equivalent to editing the script "
                             "in the scripted brush dialog. Prefer run_script "
                             "for one-shot execution."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("script"),
                          obj({{QStringLiteral("type"), str}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("script")}}})});

    t.append({QStringLiteral("get_terrain_at"),
              QStringLiteral("Terrain sample at (x, y) in map cells: geo "
                             "height, dam height (0 where no dam layer), the "
                             "geo/dam palette indices and RGB colors of the "
                             "cell, and the raw surface attribute bitmask "
                             "(zero layer, soot, etc.)."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("x"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("y"),
                          obj({{QStringLiteral("type"), integer}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("x"), QStringLiteral("y")}}})});

    {
        // Built step by step — nested schema literals are brace-count
        // minefields (see the git history of this file).
        QJsonObject paletteProp;
        paletteProp.insert(QStringLiteral("type"), str);
        paletteProp.insert(QStringLiteral("enum"),
                           QJsonArray{QStringLiteral("geo"),
                                      QStringLiteral("dam")});
        QJsonObject paletteProperties;
        paletteProperties.insert(QStringLiteral("palette"), paletteProp);
        t.append({QStringLiteral("get_palette"),
                  QStringLiteral("Returns all 256 colors of a palette: index "
                                 "and r/g/b per entry."),
                  obj({{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), paletteProperties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("palette")}}})});
    }

    {
        QJsonObject paletteProp2;
        paletteProp2.insert(QStringLiteral("type"), str);
        paletteProp2.insert(QStringLiteral("enum"),
                            QJsonArray{QStringLiteral("geo"),
                                       QStringLiteral("dam")});
        QJsonObject shapeProperties;
        shapeProperties.insert(QStringLiteral("palette"), paletteProp2);
        shapeProperties.insert(
            QStringLiteral("index"),
            obj({{QStringLiteral("type"), integer}}));
        shapeProperties.insert(QStringLiteral("r"),
                               obj({{QStringLiteral("type"), integer}}));
        shapeProperties.insert(QStringLiteral("g"),
                               obj({{QStringLiteral("type"), integer}}));
        shapeProperties.insert(QStringLiteral("b"),
                               obj({{QStringLiteral("type"), integer}}));
        t.append({QStringLiteral("set_palette_color"),
                  QStringLiteral("Rewrites one palette entry (0..255) of the "
                                 "geo or dam palette and rebuilds the render "
                                 "tables, exactly like the palette panel.") +
                      undoNote,
                  obj({{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), shapeProperties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("palette"),
                                   QStringLiteral("index"),
                                   QStringLiteral("r"), QStringLiteral("g"),
                                   QStringLiteral("b")}}})});
    }

    t.append({QStringLiteral("load_decal_image"),
              QStringLiteral("Loads an image (PNG/TGA/BMP, max 512 px per "
                             "side) as the ImageDecal brush stamp: the "
                             "pixels are matched against the Dam palette "
                             "(new colours are written into it), and every "
                             "fully opaque pixel paints its palette entry. "
                             "Apply with apply_brush effect 'imageDecal'. "
                             "Dam-layer only.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("file"),
                          obj({{QStringLiteral("type"), str}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("file")}}})});

    t.append({QStringLiteral("import_model"),
              QStringLiteral("Imports a PLY model (ASCII or binary) as the "
                             "Model brush stamp: the model is treated as a "
                             "solid heightmap - per placement cell the "
                             "height and the top-part colour are extracted, "
                             "and the colours are quantized into the Dam "
                             "palette. Apply with apply_brush effect "
                             "'model' (modelHeight/scaleX/scaleY/mode). "
                             "Dam-layer only.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("file"),
                          obj({{QStringLiteral("type"), str}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("file")}}})});

    t.append({QStringLiteral("set_difference_mode"),
              QStringLiteral("Enters (enabled=true) or exits "
                             "(enabled=false) the difference mode, like the "
                             "editor's F2 toggle. ON: the terrain shows the "
                             "base world with the .gmp difference applied, "
                             "and every apply_brush stamp is captured as "
                             "difference data (saved via save_difference). "
                             "OFF: the plain base world. Switching clears "
                             "the undo history."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("enabled"),
                          obj({{QStringLiteral("type"), boolean}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("enabled")}}})});

    QJsonObject filePropDiff;
    filePropDiff.insert(QStringLiteral("type"), str);
    QJsonObject diffFileProperties;
    diffFileProperties.insert(QStringLiteral("file"), filePropDiff);

    t.append({QStringLiteral("save_difference"),
              QStringLiteral("Writes the difference model to a .gmp file in "
                             "the game's format (needed to change the "
                             "terrain per battle mode, e.g. skirmish or "
                             "survival). Without 'file' the loaded battle "
                             "map's .gmp sibling is used. Works in and out "
                             "of the difference mode."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), diffFileProperties}})});

    t.append({QStringLiteral("load_difference"),
              QStringLiteral("Replaces the difference model with a .gmp "
                             "file (the game's format). Without 'file' the "
                             "loaded battle map's .gmp sibling is used when "
                             "it exists. The terrain view updates when the "
                             "difference mode is ON."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), diffFileProperties}})});

    t.append({QStringLiteral("list_players"),
              QStringLiteral("Lists the players of the loaded battle map: "
                             "id, name, realPlayerType (REAL_PLAYER_TYPE_*), "
                             "belligerent (BELLIGERENT_*), colorIndex, clan, "
                             "difficulty, handicap and the spawn anchor "
                             "(x, y)."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), QJsonObject()}})});

    t.append({QStringLiteral("update_player"),
              QStringLiteral("Edits one player of the loaded battle map "
                             "(index from list_players): any of name, "
                             "realPlayerType, belligerent, colorIndex, clan, "
                             "difficulty, handicap; passing x/y relocates "
                             "the whole spawn group (frame, squad, camera) "
                             "with the terrain-height correction, exactly "
                             "like the Edit Properties dialog.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("index"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("name"),
                          obj({{QStringLiteral("type"), str}})},
                         {QStringLiteral("realPlayerType"),
                          obj({{QStringLiteral("type"), str}})},
                         {QStringLiteral("belligerent"),
                          obj({{QStringLiteral("type"), str}})},
                         {QStringLiteral("colorIndex"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("clan"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("difficulty"),
                          obj({{QStringLiteral("type"), str}})},
                         {QStringLiteral("handicap"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("x"),
                          obj({{QStringLiteral("type"), num}})},
                         {QStringLiteral("y"),
                          obj({{QStringLiteral("type"), num}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("index")}}})});

    t.append({QStringLiteral("add_player"),
              QStringLiteral("Adds one player to the loaded battle map: the "
                             "new entry is cloned from the last existing "
                             "player (spawn group included), then any "
                             "provided properties are applied. Optional x/y "
                             "place the spawn anchor (with the "
                             "terrain-height correction). Use "
                             "update_player afterwards for fine edits.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("name"),
                          obj({{QStringLiteral("type"), str}})},
                         {QStringLiteral("realPlayerType"),
                          obj({{QStringLiteral("type"), str}})},
                         {QStringLiteral("belligerent"),
                          obj({{QStringLiteral("type"), str}})},
                         {QStringLiteral("colorIndex"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("clan"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("difficulty"),
                          obj({{QStringLiteral("type"), str}})},
                         {QStringLiteral("handicap"),
                          obj({{QStringLiteral("type"), integer}})},
                         {QStringLiteral("x"),
                          obj({{QStringLiteral("type"), num}})},
                         {QStringLiteral("y"),
                          obj({{QStringLiteral("type"), num}})}})}})});

    t.append({QStringLiteral("remove_player"),
              QStringLiteral("Removes one player (index from list_players) "
                             "from the loaded battle map together with its "
                             "spawn group and camera; the remaining ids are "
                             "renumbered. The last player cannot be "
                             "removed.") +
                  undoNote,
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"),
                    obj({{QStringLiteral("index"),
                          obj({{QStringLiteral("type"), integer}})}})},
                   {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("index")}}})});

    t.append({QStringLiteral("undo"),
              QStringLiteral("Reverts the last undo transaction: a single "
                             "brush stamp, an object placement/edit, a "
                             "palette rewrite, a script run, a world "
                             "creation. NOTE: the editor groups consecutive "
                             "edits applied within 500 ms into ONE "
                             "transaction — rapid successive calls may undo "
                             "together."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), QJsonObject()}})});

    t.append({QStringLiteral("redo"),
              QStringLiteral("Re-applies the last undone transaction. Same "
                             "500 ms grouping note as undo."),
              obj({{QStringLiteral("type"), QStringLiteral("object")},
                   {QStringLiteral("properties"), QJsonObject()}})});

    return t;
}

} // namespace MapEditor
