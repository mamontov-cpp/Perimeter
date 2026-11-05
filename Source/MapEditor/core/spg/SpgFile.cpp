// ============================================================================
// MapEditorCore — SpgFile implementation (T8.1).
// ============================================================================

#include "SpgFile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace MapEditor {

namespace {

// --- scalar conversions -----------------------------------------------------

std::string unquote(const std::string& raw)
{
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        std::string out;
        for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 2 < raw.size() + 1 &&
                (raw[i + 1] == '"' || raw[i + 1] == '\\')) {
                ++i;
            }
            out += raw[i];
        }
        return out;
    }
    return raw;
}

std::string quote(const std::string& text)
{
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
    return out;
}

bool toDouble(const std::string& raw, double& value)
{
    if (raw.empty()) {
        return false;
    }
    char* end = nullptr;
    value = std::strtod(raw.c_str(), &end);
    return end != raw.c_str();
}

bool toInt(const std::string& raw, int& value)
{
    if (raw.empty()) {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str()) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

std::string num(double value)
{
    // %.6g — the same magnitude of precision the game's XBuffer writer
    // produces (386.768, 1.33475e-004, 1000.).
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

std::string num(int value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d", value);
    return buffer;
}

// --- schema helpers ---------------------------------------------------------

/// Vector3 objects ("position = { x = ...; y = ...; z = ...; }").
bool readVec3(XPrmObject* parent, const char* key, double& x, double& y,
              double& z, bool& hasZ)
{
    XPrmObject* vec = parent ? parent->object(key) : nullptr;
    if (!vec) {
        return false;
    }
    if (!toDouble(vec->scalar("x"), x) || !toDouble(vec->scalar("y"), y)) {
        return false;
    }
    hasZ = toDouble(vec->scalar("z"), z);
    return true;
}

void shiftVec(XPrmObject* parent, const char* key, double dx, double dy,
              double dz, bool hasZAxis)
{
    XPrmObject* vec = parent ? parent->object(key) : nullptr;
    if (!vec) {
        return;
    }
    double x = 0;
    double y = 0;
    double z = 0;
    if (toDouble(vec->scalar("x"), x)) {
        vec->setScalar("x", num(x + dx));
    }
    if (toDouble(vec->scalar("y"), y)) {
        vec->setScalar("y", num(y + dy));
    }
    if (hasZAxis && toDouble(vec->scalar("z"), z)) {
        vec->setScalar("z", num(z + dz));
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

bool SpgFile::load(const std::string& path)
{
    mission_ = nullptr;
    save_ = nullptr;
    textEncoding_ = SpgTextEncoding::Utf8;
    clearPlayerMoveHistory();
    if (!doc_.load(path)) {
        return false;
    }
    const XPrmDocument::Block* missionBlock = doc_.block("MissionDescriptionPrm");
    const XPrmDocument::Block* saveBlock = doc_.block("SavePrm");
    mission_ = missionBlock ? missionBlock->object.get() : nullptr;
    save_ = saveBlock ? saveBlock->object.get() : nullptr;

    // Detect the encoding of the editable strings from the player names.
    // The document itself keeps raw bytes; the transcode happens at the
    // playerData()/setPlayerData() boundary.
    if (loaded()) {
        std::vector<std::string> names;
        for (int i = 0; i < playerCount(); ++i) {
            names.push_back(
                unquote(elementObject(playersDataArray(), i)
                            ->scalar("name")));
        }
        textEncoding_ = detectSpgTextEncoding(names);
    }
    return loaded();
}

bool SpgFile::save(const std::string& path) const
{
    return doc_.save(path);
}

// ---------------------------------------------------------------------------
// Access helpers
// ---------------------------------------------------------------------------

XPrmObject* SpgFile::playersDataArray()
{
    return mission_ ? mission_->object("playersData") : nullptr;
}

XPrmObject* SpgFile::playersArray()
{
    return save_ ? save_->object("players") : nullptr;
}

XPrmObject* SpgFile::camerasArray()
{
    XPrmObject* manual = save_ ? save_->object("manualData") : nullptr;
    return manual ? manual->object("cameras") : nullptr;
}

XPrmObject* SpgFile::elementObject(XPrmObject* array, int index)
{
    if (!array || index < 0 ||
        static_cast<size_t>(index) >= array->elementCount()) {
        return nullptr;
    }
    XPrmObject::Field& field = array->fields[static_cast<size_t>(index) + 1];
    return field.hasObject ? field.object.get() : nullptr;
}

XPrmObject* SpgFile::playerFrame(int playerIndex)
{
    XPrmObject* player = elementObject(playersArray(), playerIndex);
    return player ? player->object("frame") : nullptr;
}

// ---------------------------------------------------------------------------
// MissionDescriptionPrm
// ---------------------------------------------------------------------------

std::string SpgFile::version() const
{
    return mission_ ? unquote(mission_->scalar("version")) : std::string();
}

std::string SpgFile::worldName() const
{
    return mission_ ? unquote(mission_->scalar("worldName")) : std::string();
}

bool SpgFile::setWorldName(const std::string& name)
{
    if (!mission_) {
        return false;
    }
    mission_->setScalar("worldName", quote(name));
    return true;
}

int SpgFile::playerCount() const
{
    const XPrmObject* array =
        const_cast<SpgFile*>(this)->playersDataArray();
    if (!array || array->fields.empty()) {
        return 0;
    }
    int count = 0;
    toInt(array->fields.front().scalar, count);
    return count;
}

SpgFile::PlayerData SpgFile::playerData(int index) const
{
    PlayerData data;
    const XPrmObject* player =
        const_cast<SpgFile*>(this)->elementObject(
            const_cast<SpgFile*>(this)->playersDataArray(), index);
    if (!player) {
        return data;
    }
    toInt(player->scalar("playerID"), data.playerID);
    data.realPlayerType = player->scalar("realPlayerType");
    data.belligerent = player->scalar("belligerent");
    toInt(player->scalar("colorIndex"), data.colorIndex);
    toInt(player->scalar("clan"), data.clan);
    data.difficulty = player->scalar("difficulty");
    toInt(player->scalar("handicap"), data.handicap);
    data.name = spgTextToUtf8(unquote(player->scalar("name")), textEncoding_);
    return data;
}

bool SpgFile::setPlayerData(int index, const PlayerData& data)
{
    XPrmObject* player = elementObject(playersDataArray(), index);
    if (!player) {
        return false;
    }
    player->setScalar("playerID", num(data.playerID));
    player->setScalar("realPlayerType", data.realPlayerType);
    player->setScalar("belligerent", data.belligerent);
    player->setScalar("colorIndex", num(data.colorIndex));
    player->setScalar("clan", num(data.clan));
    player->setScalar("difficulty", data.difficulty);
    player->setScalar("handicap", num(data.handicap));
    player->setScalar("name",
                      quote(utf8ToSpgText(data.name, textEncoding_)));
    return true;
}

std::vector<int> SpgFile::shufflingIndices() const
{
    std::vector<int> indices;
    const XPrmObject* array =
        const_cast<SpgFile*>(this)->mission_
            ? const_cast<SpgFile*>(this)->mission_->object(
                  "playersShufflingIndices")
            : nullptr;
    if (!array || array->fields.empty()) {
        return indices;
    }
    for (std::size_t i = 1; i < array->fields.size(); ++i) {
        int value = 0;
        if (toInt(array->fields[i].scalar, value)) {
            indices.push_back(value);
        }
    }
    return indices;
}

bool SpgFile::setShufflingIndices(const std::vector<int>& indices)
{
    XPrmObject* array =
        mission_ ? mission_->object("playersShufflingIndices") : nullptr;
    if (!array) {
        return false;
    }
    // Rebuild the array body: count entry + one scalar per index.
    array->fields.clear();
    array->gaps.clear();
    array->gaps.emplace_back(" ");
    XPrmObject::Field count;
    count.scalar = num(static_cast<int>(indices.size()));
    count.terminator = ';';
    count.edited = true;
    array->fields.push_back(std::move(count));
    array->gaps.emplace_back(" ");
    for (std::size_t i = 0; i < indices.size(); ++i) {
        XPrmObject::Field entry;
        entry.scalar = num(indices[i]);
        entry.terminator = i + 1 < indices.size() ? ',' : 0;
        entry.edited = true;
        array->fields.push_back(std::move(entry));
        array->gaps.emplace_back(" ");
    }
    return true;
}

// ---------------------------------------------------------------------------
// SavePrm
// ---------------------------------------------------------------------------

int SpgFile::zeroLayerHeight() const
{
    const XPrmObject* manual =
        const_cast<SpgFile*>(this)->save_
            ? const_cast<SpgFile*>(this)->save_->object("manualData")
            : nullptr;
    int height = 0;
    if (manual) {
        toInt(manual->scalar("zeroLayerHeight"), height);
    }
    return height;
}

bool SpgFile::setZeroLayerHeight(int height)
{
    XPrmObject* manual = save_ ? save_->object("manualData") : nullptr;
    if (!manual) {
        return false;
    }
    manual->setScalar("zeroLayerHeight", num(height));
    return true;
}

bool SpgFile::playerAnchor(int playerIndex, double& x, double& y) const
{
    XPrmObject* frame = const_cast<SpgFile*>(this)->playerFrame(playerIndex);
    double z = 0;
    bool hasZ = false;
    return readVec3(frame, "position", x, y, z, hasZ);
}

bool SpgFile::playerCamera(int playerIndex, double& x, double& y,
                           double& psi, double& theta, double& distance) const
{
    SpgFile* self = const_cast<SpgFile*>(this);
    XPrmObject* spline = elementObject(self->camerasArray(), playerIndex);
    if (!spline) {
        return false;
    }
    XPrmObject* path = spline->object("path");
    XPrmObject* point = elementObject(path, 0);
    if (!point) {
        return false;
    }
    double z = 0;
    bool hasZ = false;
    if (!readVec3(point, "position", x, y, z, hasZ)) {
        return false;
    }
    if (!toDouble(point->scalar("psi"), psi) ||
        !toDouble(point->scalar("theta"), theta) ||
        !toDouble(point->scalar("distance"), distance)) {
        return false;
    }
    return true;
}

bool SpgFile::movePlayerGroup(int playerIndex, double dx, double dy,
                              double dz)
{
    XPrmObject* frame = playerFrame(playerIndex);
    if (!frame) {
        return false;
    }
    shiftVec(frame, "position", dx, dy, dz, true);

    // Squad + its stable ground position.
    XPrmObject* squad = frame->object("squad");
    shiftVec(squad, "position", dx, dy, dz, true);
    shiftVec(squad, "stablePosition", dx, dy, 0, false);

    // Frame slots (child units attached to the frame).
    XPrmObject* slots = frame->object("frameSlots");
    if (slots) {
        for (std::size_t i = 1; i < slots->fields.size(); ++i) {
            XPrmObject::Field& slotField = slots->fields[i];
            XPrmObject* slot =
                slotField.hasObject ? slotField.object.get() : nullptr;
            shiftVec(slot, "position", dx, dy, dz, true);
        }
    }

    // The per-player camera: shift every path point (2D only).
    XPrmObject* cameras = camerasArray();
    XPrmObject* camera = elementObject(cameras, playerIndex);
    if (camera) {
        XPrmObject* path = camera->object("path");
        if (path) {
            for (std::size_t i = 1; i < path->fields.size(); ++i) {
                XPrmObject::Field& pointField = path->fields[i];
                XPrmObject* point =
                    pointField.hasObject ? pointField.object.get() : nullptr;
                shiftVec(point, "position", dx, dy, 0, false);
            }
        }
    }
    return true;
}

bool SpgFile::setPlayerAnchor(int playerIndex, double x, double y, double dz)
{
    double oldX = 0;
    double oldY = 0;
    if (!playerAnchor(playerIndex, oldX, oldY)) {
        return false;
    }
    return movePlayerGroup(playerIndex, x - oldX, y - oldY, dz);
}

bool SpgFile::setPlayerAnchorUndoable(int playerIndex, double x, double y,
                                      double dz)
{
    double oldX = 0;
    double oldY = 0;
    if (!playerAnchor(playerIndex, oldX, oldY)) {
        return false;
    }
    const double dx = x - oldX;
    const double dy = y - oldY;
    if (!movePlayerGroup(playerIndex, dx, dy, dz)) {
        return false;
    }
    playerMoveUndo_.push_back(PlayerMoveStep{playerIndex, dx, dy, dz});
    playerMoveRedo_.clear();
    return true;
}

bool SpgFile::undoPlayerMove()
{
    if (playerMoveUndo_.empty()) {
        return false;
    }
    const PlayerMoveStep step = playerMoveUndo_.back();
    playerMoveUndo_.pop_back();
    movePlayerGroup(step.playerIndex, -step.dx, -step.dy, -step.dz);
    playerMoveRedo_.push_back(step);
    return true;
}

bool SpgFile::redoPlayerMove()
{
    if (playerMoveRedo_.empty()) {
        return false;
    }
    const PlayerMoveStep step = playerMoveRedo_.back();
    playerMoveRedo_.pop_back();
    movePlayerGroup(step.playerIndex, step.dx, step.dy, step.dz);
    playerMoveUndo_.push_back(step);
    return true;
}

void SpgFile::clearPlayerMoveHistory()
{
    playerMoveUndo_.clear();
    playerMoveRedo_.clear();
}

bool SpgFile::restPlayersOnZeroLayer(
    int zeroLayerHeight,
    const std::function<int(int, int)>& terrainHeight)
{
    if (!loaded()) {
        return false;
    }
    constexpr double kTolerance = 0.5;
    constexpr double kSquadPullDownTolerance = 2.0;

    for (int i = 0; i < playerCount(); ++i) {
        XPrmObject* frame = playerFrame(i);
        if (!frame) {
            continue;
        }
        XPrmObject* squad = frame->object("squad");

        // Squad ground level: pull down to the zero layer only — a squad
        // resting on low terrain below the zero layer stays put (the game's
        // landing check is one-sided and counts it as landed).
        double sx = 0;
        double sy = 0;
        double sz = 0;
        bool squadHasZ = false;
        if (readVec3(squad, "position", sx, sy, sz, squadHasZ) && squadHasZ &&
            sz - zeroLayerHeight > kSquadPullDownTolerance) {
            squad->object("position")->setScalar("z", num(zeroLayerHeight));
        }

        // Frame grounded pose: the game installs at the stored z (the body
        // has no gravity), so the stored z must be the grounded pose —
        // terrain under the frame + the model's origin offset. The packed
        // buildings ride with the frame.
        double fx = 0;
        double fy = 0;
        double fz = 0;
        bool frameHasZ = false;
        if (!readVec3(frame, "position", fx, fy, fz, frameHasZ) || !frameHasZ) {
            continue;
        }
        int ground = terrainHeight
                         ? terrainHeight(static_cast<int>(fx), static_cast<int>(fy))
                         : -1;
        if (ground < 0) {
            ground = zeroLayerHeight; // off-map fallback
        }
        const double target = ground + kFrameOriginGroundOffset;
        if (std::abs(fz - target) <= kTolerance) {
            continue;
        }
        const double delta = target - fz;
        frame->object("position")->setScalar("z", num(target));

        XPrmObject* slots = frame->object("frameSlots");
        if (slots) {
            for (std::size_t k = 1; k < slots->fields.size(); ++k) {
                XPrmObject* slot =
                    slots->fields[k].hasObject ? slots->fields[k].object.get()
                                               : nullptr;
                double zx = 0;
                double zy = 0;
                double zz = 0;
                bool slotHasZ = false;
                if (readVec3(slot, "position", zx, zy, zz, slotHasZ) && slotHasZ) {
                    slot->object("position")->setScalar("z", num(zz + delta));
                }
            }
        }
    }
    return true;
}

namespace {

/// Collects the values of every `zeroLayerCounter` scalar in the tree —
/// the ids must stay unique document-wide (loadZeroLayer keys a std::map
/// by them, duplicates would silently drop a unit's zeroplast disc).
void collectZeroLayerCounters(const XPrmObject* obj, std::vector<int>& out)
{
    if (!obj) {
        return;
    }
    for (const XPrmObject::Field& field : obj->fields) {
        if (field.hasObject) {
            collectZeroLayerCounters(field.object.get(), out);
        } else if (field.name == "zeroLayerCounter") {
            int value = 0;
            if (toInt(field.scalar, value) && value > 0) {
                out.push_back(value);
            }
        }
    }
}

} // namespace

bool SpgFile::setFramesInstalledForStart()
{
    if (!loaded()) {
        return false;
    }
    std::vector<int> used;
    collectZeroLayerCounters(save_, used);

    int next = 1;
    for (int i = 0; i < playerCount(); ++i) {
        XPrmObject* frame = playerFrame(i);
        if (!frame) {
            continue;
        }
        frame->setScalar("attached", "true");
        frame->setScalar("powered", "true");
        int id = 0;
        toInt(frame->scalar("zeroLayerCounter"), id);
        if (id <= 0) {
            // Fresh disc: assign the lowest unused id, like the game's own
            // zeroLayerCounterCurrent_ increment does at install time.
            while (std::find(used.begin(), used.end(), next) != used.end()) {
                ++next;
            }
            id = next++;
            frame->setScalar("zeroLayerCounter", num(id));
        }
        used.push_back(id);
    }
    return true;
}

// ---------------------------------------------------------------------------
// T8.4 — setPlayerCount
// ---------------------------------------------------------------------------

bool SpgFile::setPlayerCount(int count)
{
    if (!loaded() || count < 1 || count > 8) {
        return false;
    }
    XPrmObject* data = playersDataArray();
    XPrmObject* players = playersArray();
    if (!data || !players || data->fields.empty() || players->fields.empty()) {
        return false;
    }

    auto resize = [count](XPrmObject* array) {
        if (!array || array->fields.empty()) {
            return;
        }
        // Deep-clone the last element until the count matches. The clone
        // takes over the separator-less tail position; the element it
        // displaces is regenerated with a ',' (see appendElement).
        while (array->elementCount() < static_cast<std::size_t>(count)) {
            array->appendElement(array->fields.back().deepCopy());
        }
        while (array->elementCount() > static_cast<std::size_t>(count)) {
            array->fields.pop_back();
            array->gaps.pop_back();
        }
        array->fields.front().scalar = num(count);
        array->fields.front().edited = true;
    };
    resize(data);
    resize(players);
    resize(camerasArray());

    // Renumber the playersData ids.
    for (int i = 0; i < count; ++i) {
        if (XPrmObject* player = elementObject(data, i)) {
            player->setScalar("playerID", num(i));
        }
    }

    mission_->setScalar("playerAmountScenarioMax", num(count));
    mission_->setScalar("activePlayerID", num(0));

    std::vector<int> identity(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        identity[static_cast<std::size_t>(i)] = i;
    }
    setShufflingIndices(identity);
    return true;
}

// ---------------------------------------------------------------------------
// Pre-placed world buildings (SavePrm.nobodysBuildings)
// ---------------------------------------------------------------------------

namespace {

/// Format a number the way the game's XPrm writer does: integral values get
/// a trailing dot ("2709."), fractional ones keep up to 6 significant
/// digits with a dot ("40.6221").
std::string formatXPrmNumber(double v)
{
    char buf[64];
    if (std::floor(v) == v && std::abs(v) < 1e15) {
        snprintf(buf, sizeof(buf), "%lld.",
                 static_cast<long long>(v));
        return buf;
    }
    snprintf(buf, sizeof(buf), "%.6g", v);
    std::string s = buf;
    if (s.find('.') == std::string::npos &&
        s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos &&
        s.find("nan") == std::string::npos) {
        s += '.';
    }
    return s;
}

XPrmObject::Field namedScalar(const std::string& name, const std::string& raw)
{
    XPrmObject::Field f;
    f.name = name;
    f.scalar = raw;
    f.terminator = ';';
    f.edited = true;
    return f;
}

XPrmObject::Field anonScalar(const std::string& raw, char terminator)
{
    XPrmObject::Field f;
    f.scalar = raw;
    f.terminator = terminator;
    f.edited = true;
    return f;
}

/// Fresh object with single-space gaps: `{ x = 1.; y = 2.; }`.
std::shared_ptr<XPrmObject> freshObject(std::vector<XPrmObject::Field> fields)
{
    auto obj = std::make_shared<XPrmObject>();
    obj->fields = std::move(fields);
    obj->gaps.assign(obj->fields.size() + 1, " ");
    return obj;
}

std::shared_ptr<XPrmObject> vectorObject(double x, double y, double z)
{
    return freshObject({namedScalar("x", formatXPrmNumber(x)),
                        namedScalar("y", formatXPrmNumber(y)),
                        namedScalar("z", formatXPrmNumber(z))});
}

/// Rotation around Z as a quaternion (the only rotation the game uses for
/// placed buildings).
std::shared_ptr<XPrmObject> quaternionZObject(double angleDeg)
{
    const double half = angleDeg * 3.14159265358979323846 / 360.0;
    return freshObject(
        {namedScalar("s", formatXPrmNumber(std::cos(half))),
         namedScalar("x", "0."), namedScalar("y", "0."),
         namedScalar("z", formatXPrmNumber(std::sin(half)))});
}

/// damageMolecula for the given HP percent: dead[i] scales with the
/// building's max element counts (UnitAttribute.prm damageMolecula).
std::shared_ptr<XPrmObject> damageObject(int hp,
                                         const BuildingInfo& info)
{
    const int clamped = std::clamp(hp, 0, 100);
    auto elements = std::make_shared<XPrmObject>();
    elements->fields.push_back(anonScalar("5", ';'));
    for (int i = 0; i < 5; ++i) {
        int dead = 0;
        if (clamped == 0) {
            dead = info.molecula[static_cast<std::size_t>(i)];
        } else if (clamped < 100) {
            dead = static_cast<int>(std::llround(
                (100 - clamped) / 100.0 *
                info.molecula[static_cast<std::size_t>(i)]));
        }
        elements->fields.push_back(
            anonScalar(std::to_string(dead), i == 4 ? '\0' : ','));
        // The last element carries NO terminator — the game's reader
        // expects '}' right after it (a ';' there aborts the parse).
    }
    elements->gaps.assign(elements->fields.size() + 1, " ");

    return freshObject(
        {namedScalar("isAlive", clamped > 0 ? "true" : "false"),
         XPrmObject::Field{/*name*/ "elementsDead",
                           /*typeTag*/ "",
                           /*hasObject*/ true,
                           /*scalar*/ "",
                           /*object*/ elements,
                           /*raw*/ "",
                           /*terminator*/ ';',
                           /*edited*/ true}});
}

/// A full "struct SaveUnit*Data" object for a constructed, plugged-in
/// building — the same defaults the shipped mission maps carry.
std::shared_ptr<XPrmObject> buildingEntryObject(const SpgFile::PlacedBuilding& b,
                                                const BuildingInfo& info)
{
    std::vector<XPrmObject::Field> fields;
    fields.push_back(namedScalar("unitID", std::to_string(b.unitId)));
    fields.push_back(namedScalar("attributeID", info.identifier));
    fields.push_back(XPrmObject::Field{"position", "", true, "", vectorObject(b.x, b.y, b.z), "", ';', true});
    fields.push_back(XPrmObject::Field{"orientaion", "", true, "", quaternionZObject(b.angleDeg), "", ';', true});
    fields.push_back(namedScalar("radius", "0."));
    fields.push_back(namedScalar("label", "\"\""));
    fields.push_back(XPrmObject::Field{"damageMolecula", "", true, "", damageObject(b.hp, info), "", ';', true});
    fields.push_back(namedScalar("basementInstalled", "true"));
    fields.push_back(namedScalar("accumulatedEnergy", "0."));
    fields.push_back(namedScalar("zeroLayerCounter", "0"));
    fields.push_back(namedScalar("weaponChargeLevel", "1."));
    fields.push_back(XPrmObject::Field{"wayPoints", "", true, "", freshObject({anonScalar("0", ';')}), "", ';', true});
    fields.push_back(namedScalar("weapon", "0"));
    if (info.kind == BuildingKind::Protector) {
        // The shipped saves carry a constructed but not-enabled Core.
        fields.push_back(namedScalar(
            "buildingStatusBV",
            "BUILDING_STATUS_CONSTRUCTED | BUILDING_STATUS_PLUGGED_IN"));
    } else {
        fields.push_back(namedScalar(
            "buildingStatusBV",
            "BUILDING_STATUS_CONSTRUCTED | BUILDING_STATUS_PLUGGED_IN | "
            "BUILDING_STATUS_ENABLED | BUILDING_STATUS_POWERED"));
    }
    fields.push_back(namedScalar("fireCount", "0"));
    fields.push_back(namedScalar("visible", "true"));
    if (info.kind == BuildingKind::CommandCenter) {
        fields.push_back(namedScalar("squad", "0"));
    } else if (info.kind == BuildingKind::Protector) {
        fields.push_back(namedScalar("monksNumber", "0"));
        fields.push_back(namedScalar("fieldState", "1"));
        fields.push_back(namedScalar("enableCharge", "true"));
        fields.push_back(namedScalar("startWhenCharged", "false"));
    }
    return freshObject(std::move(fields));
}

/// The nobodysBuildings.objects array (created when absent so a fresh
/// battle map can take buildings too).
XPrmObject* buildingsArray(XPrmObject* save, bool create)
{
    if (!save) {
        return nullptr;
    }
    XPrmObject* holder = save->object("nobodysBuildings");
    if (!holder && create) {
        auto objects = std::make_shared<XPrmObject>();
        objects->fields.push_back(anonScalar("0", ';'));
        objects->gaps.assign(2, " ");
        auto block = std::make_shared<XPrmObject>();
        block->fields.push_back(XPrmObject::Field{"objects", "", true, "", objects, "", ';', true});
        block->gaps.assign(2, " ");
        XPrmObject::Field entry;
        entry.name = "nobodysBuildings";
        entry.hasObject = true;
        entry.object = block;
        entry.terminator = ';';
        entry.edited = true;
        save->addField(std::move(entry));
        holder = save->object("nobodysBuildings");
    }
    return holder ? holder->object("objects") : nullptr;
}

double scalarDouble(const XPrmObject* obj, const char* name)
{
    if (!obj) {
        return 0.0;
    }
    return std::strtod(obj->scalar(name).c_str(), nullptr);
}

int scalarInt(const XPrmObject* obj, const char* name)
{
    if (!obj) {
        return 0;
    }
    return std::atoi(obj->scalar(name).c_str());
}

/// Recursively collects the largest unitID under `obj`.
void collectMaxUnitId(const XPrmObject* obj, int& maxId)
{
    if (!obj) {
        return;
    }
    for (const XPrmObject::Field& f : obj->fields) {
        if (f.name == "unitID" && f.isScalar()) {
            maxId = std::max(maxId, std::atoi(f.scalar.c_str()));
        }
        if (f.hasObject && f.object) {
            collectMaxUnitId(f.object.get(), maxId);
        }
    }
}

} // namespace

std::vector<SpgFile::PlacedBuilding> SpgFile::buildings() const
{
    std::vector<PlacedBuilding> out;
    if (!loaded()) {
        return out;
    }
    XPrmObject* array = buildingsArray(save_, false);
    if (!array) {
        return out;
    }
    for (std::size_t i = 1; i < array->fields.size(); ++i) {
        const XPrmObject::Field& entry = array->fields[i];
        if (!entry.hasObject || !entry.object) {
            continue;
        }
        const XPrmObject* obj = entry.object.get();
        const std::string attrText = obj->scalar("attributeID");
        int attributeId = -1;
        for (std::size_t k = 0; k < kPlaceableBuildingCount; ++k) {
            if (attrText == kPlaceableBuildings[k].identifier) {
                attributeId = kPlaceableBuildings[k].id;
                break;
            }
        }
        if (attributeId < 0) {
            continue; // not a placeable building — left untouched
        }
        const BuildingInfo* info = buildingInfo(attributeId);
        PlacedBuilding b;
        b.index = i - 1;
        b.unitId = scalarInt(obj, "unitID");
        b.attributeId = attributeId;
        b.x = scalarDouble(obj->object("position"), "x");
        b.y = scalarDouble(obj->object("position"), "y");
        b.z = scalarDouble(obj->object("position"), "z");
        const XPrmObject* quat = obj->object("orientaion");
        const double qs = scalarDouble(quat, "s");
        const double qz = scalarDouble(quat, "z");
        double angle = 2.0 * std::atan2(qz, qs) * 180.0 /
                       3.14159265358979323846;
        // Normalize into (-180, 180].
        while (angle > 180.0) angle -= 360.0;
        while (angle <= -180.0) angle += 360.0;
        b.angleDeg = angle;
        int hp = 100;
        const XPrmObject* damage = obj->object("damageMolecula");
        if (damage) {
            const XPrmObject* elements = damage->object("elementsDead");
            int deadSum = 0;
            int maxSum = 0;
            for (std::size_t k = 0; k < 5; ++k) {
                maxSum += info->molecula[k];
            }
            if (elements && elements->fields.size() >= 6) {
                for (std::size_t k = 1; k <= 5; ++k) {
                    deadSum += std::atoi(elements->fields[k].scalar.c_str());
                }
            }
            if (damage->scalar("isAlive") == "false") {
                hp = 0;
            } else if (maxSum > 0) {
                hp = std::clamp(static_cast<int>(std::llround(
                     100.0 * (1.0 - static_cast<double>(deadSum) / maxSum))),
                     0, 100);
            }
        }
        b.hp = hp;
        out.push_back(b);
    }
    return out;
}

bool SpgFile::addBuilding(const PlacedBuilding& building)
{
    if (!loaded()) {
        return false;
    }
    const BuildingInfo* info = buildingInfo(building.attributeId);
    XPrmObject* array = buildingsArray(save_, true);
    if (!info || !array) {
        return false;
    }
    if (!array->fields.empty() && array->fields.back().terminator == 0) {
        // The previous last element gains a separator before the newcomer.
        array->fields.back().terminator = ',';
        array->fields.back().edited = true;
    }
    XPrmObject::Field entry;
    entry.typeTag =
        info->kind == BuildingKind::Protector
            ? "struct SaveUnitProtectorData"
            : info->kind == BuildingKind::CommandCenter
                  ? "struct SaveUnitCommandCenterData"
                  : "struct SaveUnitBuildingData";
    entry.hasObject = true;
    entry.object = buildingEntryObject(building, *info);
    entry.edited = true; // fresh object — never render the (empty) raw slice
    array->appendElement(std::move(entry));
    if (!array->fields.empty()) {
        array->fields[0].scalar = std::to_string(array->elementCount());
        array->fields[0].edited = true;
    }
    return true;
}

bool SpgFile::setBuildingPose(std::size_t index, double x, double y,
                              double angleDeg)
{
    if (!loaded()) {
        return false;
    }
    XPrmObject* array = buildingsArray(save_, false);
    if (!array || index + 1 >= array->fields.size() ||
        !array->fields[index + 1].object) {
        return false;
    }
    XPrmObject* obj = array->fields[index + 1].object.get();
    XPrmObject* position = obj->object("position");
    if (!position) {
        return false;
    }
    position->setScalar("x", formatXPrmNumber(x));
    position->setScalar("y", formatXPrmNumber(y));
    position->setScalar("z",
                        formatXPrmNumber(scalarDouble(position, "z")));
    // Replace the whole quaternion object.
    XPrmObject::Field* quatField = obj->field("orientaion");
    if (!quatField) {
        return false;
    }
    quatField->hasObject = true;
    quatField->object = quaternionZObject(angleDeg);
    quatField->edited = true;
    return true;
}

bool SpgFile::setBuildingHp(std::size_t index, int hp)
{
    if (!loaded()) {
        return false;
    }
    XPrmObject* array = buildingsArray(save_, false);
    if (!array || index + 1 >= array->fields.size() ||
        !array->fields[index + 1].object) {
        return false;
    }
    XPrmObject* obj = array->fields[index + 1].object.get();
    const std::string attrText = obj->scalar("attributeID");
    int attributeId = -1;
    for (std::size_t k = 0; k < kPlaceableBuildingCount; ++k) {
        if (attrText == kPlaceableBuildings[k].identifier) {
            attributeId = kPlaceableBuildings[k].id;
            break;
        }
    }
    const BuildingInfo* info = buildingInfo(attributeId);
    XPrmObject::Field* damageField = obj->field("damageMolecula");
    if (!info || !damageField) {
        return false;
    }
    damageField->hasObject = true;
    damageField->object = damageObject(hp, *info);
    damageField->edited = true;
    return true;
}

bool SpgFile::removeBuilding(std::size_t index)
{
    if (!loaded()) {
        return false;
    }
    XPrmObject* array = buildingsArray(save_, false);
    if (!array || index + 1 >= array->fields.size()) {
        return false;
    }
    array->fields.erase(array->fields.begin() +
                        static_cast<long>(index) + 1);
    array->gaps.erase(array->gaps.begin() + static_cast<long>(index) + 1);
    if (!array->fields.empty()) {
        // The new last element must not carry a separator before '}' —
        // regenerate it without (its raw slice may still hold ','). When no
        // elements remain fields.back() IS the count scalar — leave its ';'.
        if (array->fields.size() > 1) {
            array->fields.back().terminator = 0;
            array->fields.back().edited = true;
        }
        array->fields[0].scalar = std::to_string(array->elementCount());
        array->fields[0].edited = true;
    }
    return true;
}

int SpgFile::maxUnitId() const
{
    int maxId = 0;
    collectMaxUnitId(save_, maxId);
    return maxId;
}

} // namespace MapEditor
