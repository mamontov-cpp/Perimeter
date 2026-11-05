// ============================================================================
// MapEditorCore — SpgFile implementation (T8.1).
// ============================================================================

#include "SpgFile.h"

#include "ScumSpotCatalog.h"

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
                                         const std::array<int, 5>& molecula)
{
    const int clamped = std::clamp(hp, 0, 100);
    auto elements = std::make_shared<XPrmObject>();
    elements->fields.push_back(anonScalar("5", ';'));
    for (int i = 0; i < 5; ++i) {
        int dead = 0;
        if (clamped == 0) {
            dead = molecula[static_cast<std::size_t>(i)];
        } else if (clamped < 100) {
            dead = static_cast<int>(std::llround(
                (100 - clamped) / 100.0 *
                molecula[static_cast<std::size_t>(i)]));
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
    fields.push_back(XPrmObject::Field{"damageMolecula", "", true, "", damageObject(b.hp, info.molecula), "", ';', true});
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

/// Moves/rotates a building entry (position x/y + yaw quaternion, z kept) —
/// shared by setBuildingPose and the scum nest pose.
void poseBuildingObject(XPrmObject* obj, double x, double y, double angleDeg)
{
    XPrmObject* position = obj->object("position");
    if (position) {
        position->setScalar("x", formatXPrmNumber(x));
        position->setScalar("y", formatXPrmNumber(y));
    }
    XPrmObject::Field* quatField = obj->field("orientaion");
    if (quatField) {
        quatField->hasObject = true;
        quatField->object = quaternionZObject(angleDeg);
        quatField->edited = true;
    }
}

/// Removes element `row` (0-based, excluding the count field) from a
/// game-grammar array: the new tail loses its separator, the count updates.
void removeArrayElement(XPrmObject* array, int row)
{
    const std::size_t fieldIndex = static_cast<std::size_t>(row) + 1;
    array->fields.erase(array->fields.begin() +
                        static_cast<long>(fieldIndex));
    array->gaps.erase(array->gaps.begin() + static_cast<long>(fieldIndex));
    if (!array->fields.empty()) {
        if (array->fields.size() > 1) {
            array->fields.back().terminator = 0;
            array->fields.back().edited = true;
        }
        array->fields[0].scalar = std::to_string(array->elementCount());
        array->fields[0].edited = true;
    }
}

void appendArrayElement(XPrmObject* array, XPrmObject::Field field)
{
    field.edited = true;
    array->appendElement(std::move(field));
    array->fields[0].scalar = std::to_string(array->elementCount());
    array->fields[0].edited = true;
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
    poseBuildingObject(obj, x, y, angleDeg);
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
    damageField->object = damageObject(hp, info->molecula);
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
    removeArrayElement(array, static_cast<int>(index));
    return true;
}

int SpgFile::maxUnitId() const
{
    int maxId = 0;
    collectMaxUnitId(save_, maxId);
    return maxId;
}

// ---------------------------------------------------------------------------
// Scum nests — paired SCUM_SPOT mounds + FILTH_SPOT spawners.
//
// Format references: shipped Missions/03ISOLAND.spg — the mounds sit in
// nobodysBuildings as plain SaveUnitBuildingData (full constructed status
// set, CONSTRUCTED|PLUGGED_IN|ENABLED|POWERED), the spawners in
// filth.objects as "struct SaveUnitFilthData" (radius 32., z on the zero
// layer, label = pairing key). The activation wiring mirrors
// Scripts/Triggers/03isoland.scr (see TriggerExport.h for the serialized
// field names).
// ---------------------------------------------------------------------------

namespace {

/// terFilthSpotID identifiers by ordinal (Source/Scripts/UnitAttribute.inl).
const char* const kFilthTypeNames[] = {
    "FILTH_SPOT_ID_NONE",
    "FILTH_SPOT_ID_ANTS",
    "FILTH_SPOT_ID_WASP",
    "FILTH_SPOT_ID_DRAGON",
    "FILTH_SPOT_ID_GHOST",
    "FILTH_SPOT_ID_EYE",
    "FILTH_SPOT_ID_CROW",
    "FILTH_SPOT_ID_DAEMON",
    "FILTH_SPOT_ID_RAT",
    "FILTH_SPOT_ID_WORM",
    "FILTH_SPOT_ID_SHARK",
    "FILTH_SPOT_ID_VOLCANO",
    "FILTH_SPOT_ID_ANTS2",
    "FILTH_SPOT_ID_SNAKE",
    "FILTH_SPOT_ID_DRAGON2",
    "FILTH_SPOT_ID_VOLCANO_SCUM_DISRUPTOR",
    "FILTH_SPOT_ID_A_ANTS",
    "FILTH_SPOT_ID_A_CROW",
    "FILTH_SPOT_ID_A_DAEMON",
    "FILTH_SPOT_ID_A_DRAGON",
    "FILTH_SPOT_ID_A_EYE",
    "FILTH_SPOT_ID_A_RAT",
    "FILTH_SPOT_ID_A_SPIDER",
    "FILTH_SPOT_ID_A_WASP",
    "FILTH_SPOT_ID_A_WORM",
};
inline constexpr int kFilthTypeCount =
    static_cast<int>(sizeof(kFilthTypeNames) / sizeof(kFilthTypeNames[0]));

const char* const kFilthAttackNames[] = {
    "FILTH_ATTACK_ALL",
    "FILTH_ATTACK_PLAYER",
    "FILTH_ATTACK_AI",
    "FILTH_ATTACK_NOSELF",
    "FILTH_ATTACK_NOSELF_NOWORLD",
};
inline constexpr int kFilthAttackCount = 5;

/// A mound pairs with the nearest spawner at most this far away.
inline constexpr double kScumPairDistance = 150.0;

const char* filthTypeName(int ordinal)
{
    if (ordinal < 0 || ordinal >= kFilthTypeCount) {
        ordinal = 0;
    }
    return kFilthTypeNames[ordinal];
}

int filthTypeOrdinal(const std::string& identifier)
{
    for (int i = 0; i < kFilthTypeCount; ++i) {
        if (identifier == kFilthTypeNames[i]) {
            return i;
        }
    }
    return 0;
}

const char* filthAttackName(int ordinal)
{
    if (ordinal < 0 || ordinal >= kFilthAttackCount) {
        ordinal = 0;
    }
    return kFilthAttackNames[ordinal];
}

int filthAttackOrdinal(const std::string& identifier)
{
    for (int i = 0; i < kFilthAttackCount; ++i) {
        if (identifier == kFilthAttackNames[i]) {
            return i;
        }
    }
    return 0;
}

/// The activatingUnit identifiers the panel offers. The game accepts any
/// terUnitAttributeID; these six cover the meaningful gameplay cases.
const char* filthActivatingName(int attributeId)
{
    switch (attributeId) {
    case 0: return "UNIT_ATTRIBUTE_NONE";
    case 1: return "UNIT_ATTRIBUTE_RELAY";
    case 2: return "UNIT_ATTRIBUTE_COLLECTOR";
    case 6: return "UNIT_ATTRIBUTE_COMMANDER";
    case 49: return "UNIT_ATTRIBUTE_FRAME";
    default: return "UNIT_ATTRIBUTE_NONE";
    }
}

int filthActivatingOrdinal(const std::string& identifier)
{
    for (int id : {0, 1, 2, 6, 49}) {
        if (identifier == filthActivatingName(id)) {
            return id;
        }
    }
    return 0;
}

std::string quoted(const std::string& raw) { return "\"" + raw + "\""; }

/// Strip the surrounding quotes of a raw scalar string ("mp-1.scr" ->
/// mp-1.scr).
std::string unquoted(const std::string& scalar)
{
    if (scalar.size() >= 2 && scalar.front() == '"' &&
        scalar.back() == '"') {
        return scalar.substr(1, scalar.size() - 2);
    }
    return scalar;
}

/// SavePrm.filth.objects (created on demand — same pattern as
/// buildingsArray).
XPrmObject* filthObjectsArray(XPrmObject* save, bool create)
{
    if (!save) {
        return nullptr;
    }
    XPrmObject* holder = save->object("filth");
    if (!holder && create) {
        auto objects = std::make_shared<XPrmObject>();
        objects->fields.push_back(anonScalar("0", ';'));
        objects->gaps.assign(2, " ");
        auto block = std::make_shared<XPrmObject>();
        block->fields.push_back(XPrmObject::Field{
            "objects", "", true, "", objects, "", ';', true});
        block->gaps.assign(2, " ");
        XPrmObject::Field entry;
        entry.name = "filth";
        entry.hasObject = true;
        entry.object = block;
        entry.terminator = ';';
        entry.edited = true;
        save->addField(std::move(entry));
        holder = save->object("filth");
    }
    return holder ? holder->object("objects") : nullptr;
}

bool scalarBool(const XPrmObject* obj, const char* name)
{
    return obj && obj->scalar(name) == "true";
}

float scalarFloat(const XPrmObject* obj, const char* name)
{
    if (!obj) {
        return 0.f;
    }
    return static_cast<float>(std::strtod(obj->scalar(name).c_str(), nullptr));
}

/// The mound entry: a plain SaveUnitBuildingData with the full constructed
/// status set (the shipped mounds carry all four flags — 03ISOLAND.spg).
/// `label` is the nest's identity label — the trigger wiring watches the
/// MOUND by it (see buildScumTriggerScript).
std::shared_ptr<XPrmObject>
scumBuildingObject(const SpgFile::ScumSpot& spot, const ScumSpotInfo& info,
                   const std::string& label)
{
    std::vector<XPrmObject::Field> fields;
    fields.push_back(namedScalar("unitID", std::to_string(spot.unitId)));
    fields.push_back(namedScalar("attributeID", info.identifier));
    fields.push_back(XPrmObject::Field{"position", "", true, "",
                                       vectorObject(spot.x, spot.y, spot.z),
                                       "", ';', true});
    fields.push_back(XPrmObject::Field{"orientaion", "", true, "",
                                       quaternionZObject(spot.angleDeg),
                                       "", ';', true});
    fields.push_back(namedScalar("radius", "0."));
    fields.push_back(namedScalar("label", quoted(label)));
    fields.push_back(XPrmObject::Field{"damageMolecula", "", true, "",
                                       damageObject(spot.hp, info.molecula),
                                       "", ';', true});
    fields.push_back(namedScalar("basementInstalled", "true"));
    fields.push_back(namedScalar("accumulatedEnergy", "0."));
    fields.push_back(namedScalar("zeroLayerCounter", "0"));
    fields.push_back(namedScalar("weaponChargeLevel", "1."));
    fields.push_back(XPrmObject::Field{"wayPoints", "", true, "",
                                       freshObject({anonScalar("0", ';')}),
                                       "", ';', true});
    fields.push_back(namedScalar("weapon", "0"));
    fields.push_back(namedScalar(
        "buildingStatusBV",
        "BUILDING_STATUS_CONSTRUCTED | BUILDING_STATUS_PLUGGED_IN | "
        "BUILDING_STATUS_ENABLED | BUILDING_STATUS_POWERED"));
    fields.push_back(namedScalar("fireCount", "0"));
    fields.push_back(namedScalar("visible", "true"));
    return freshObject(std::move(fields));
}

/// The paired spawner entry: "struct SaveUnitFilthData" exactly as the
/// game's writer serializes it (shipped 03ISOLAND.spg). `z` is the zero
/// layer height (the spawners sit ON the layer).
std::shared_ptr<XPrmObject> filthEntryObject(int unitId, double x, double y,
                                             double z,
                                             const std::string& label,
                                             const SpgFile::FilthParams& p)
{
    std::vector<XPrmObject::Field> fields;
    fields.push_back(namedScalar("unitID", std::to_string(unitId)));
    fields.push_back(
        namedScalar("attributeID", "UNIT_ATTRIBUTE_FILTH_SPOT"));
    fields.push_back(XPrmObject::Field{
        "position", "", true, "", vectorObject(x, y, z), "", ';', true});
    fields.push_back(XPrmObject::Field{
        "orientaion", "", true, "",
        freshObject({namedScalar("s", "1."), namedScalar("x", "0."),
                     namedScalar("y", "0."), namedScalar("z", "0.")}),
        "", ';', true});
    fields.push_back(namedScalar("radius", "32."));
    fields.push_back(namedScalar("label", quoted(label)));
    fields.push_back(XPrmObject::Field{"damageMolecula", "", true, "",
                                       damageObject(100, {0, 0, 0, 0, 1}),
                                       "", ';', true});
    fields.push_back(namedScalar("filthType", filthTypeName(p.filthType)));
    fields.push_back(
        namedScalar("attackWidth", formatXPrmNumber(p.attackWidth)));
    fields.push_back(namedScalar(
        "attackDirection", formatXPrmNumber(p.attackDirection)));
    fields.push_back(namedScalar("sleep", p.sleep ? "true" : "false"));
    fields.push_back(namedScalar(
        "firstSleepTime", formatXPrmNumber(p.firstSleepTime)));
    fields.push_back(
        namedScalar("sleepPeriod", formatXPrmNumber(p.sleepPeriod)));
    fields.push_back(namedScalar(
        "deltaSleepPeriod", formatXPrmNumber(p.deltaSleepPeriod)));
    fields.push_back(
        namedScalar("attackPeriond", formatXPrmNumber(p.attackPeriond)));
    fields.push_back(namedScalar(
        "deltaAttackPeriond", formatXPrmNumber(p.deltaAttackPeriond)));
    fields.push_back(
        namedScalar("creatureNum", std::to_string(p.creatureNum)));
    fields.push_back(namedScalar("activatingUnit",
                                 filthActivatingName(p.activatingUnit)));
    fields.push_back(namedScalar(
        "activatingDistance", formatXPrmNumber(p.activatingDistance)));
    fields.push_back(
        namedScalar("attack_player", filthAttackName(p.attackPlayer)));
    fields.push_back(namedScalar("initial_geoprocess",
                                 p.initialGeoprocess ? "true" : "false"));
    fields.push_back(namedScalar("killTimer", "0"));
    fields.push_back(namedScalar("sleep_timer", "0"));
    fields.push_back(
        namedScalar("create_first", p.createFirst ? "true" : "false"));
    fields.push_back(XPrmObject::Field{"hole_position", "", true, "",
                                       freshObject({anonScalar("0", ';')}),
                                       "", ';', true});
    fields.push_back(XPrmObject::Field{"hole_position_inited", "", true, "",
                                       freshObject({anonScalar("0", ';')}),
                                       "", ';', true});
    fields.push_back(namedScalar("kill_of_end", "false"));
    fields.push_back(XPrmObject::Field{"swarmList", "", true, "",
                                       freshObject({anonScalar("0", ';')}),
                                       "", ';', true});
    return freshObject(std::move(fields));
}

struct ParsedFilth
{
    int fieldIndex = -1; ///< element index in filth.objects.
    int unitId = 0;
    double x = 0, y = 0;
    std::string label;
    SpgFile::FilthParams params;
};

bool parseFilthEntry(const XPrmObject* array, std::size_t fieldIndex,
                     ParsedFilth& out)
{
    const XPrmObject::Field& entry = array->fields[fieldIndex];
    if (!entry.hasObject || !entry.object) {
        return false;
    }
    const XPrmObject* obj = entry.object.get();
    if (obj->scalar("attributeID") != "UNIT_ATTRIBUTE_FILTH_SPOT") {
        return false;
    }
    out.fieldIndex = static_cast<int>(fieldIndex) - 1;
    out.unitId = scalarInt(obj, "unitID");
    const XPrmObject* pos = obj->object("position");
    out.x = scalarDouble(pos, "x");
    out.y = scalarDouble(pos, "y");
    out.label = unquoted(obj->scalar("label"));
    SpgFile::FilthParams& p = out.params;
    p.filthType = filthTypeOrdinal(obj->scalar("filthType"));
    p.attackWidth = scalarFloat(obj, "attackWidth");
    p.attackDirection = scalarFloat(obj, "attackDirection");
    p.sleep = scalarBool(obj, "sleep");
    p.firstSleepTime = scalarFloat(obj, "firstSleepTime");
    p.sleepPeriod = scalarFloat(obj, "sleepPeriod");
    p.deltaSleepPeriod = scalarFloat(obj, "deltaSleepPeriod");
    p.attackPeriond = scalarFloat(obj, "attackPeriond");
    p.deltaAttackPeriond = scalarFloat(obj, "deltaAttackPeriond");
    p.creatureNum = scalarInt(obj, "creatureNum");
    p.activatingUnit = filthActivatingOrdinal(obj->scalar("activatingUnit"));
    p.activatingDistance = scalarFloat(obj, "activatingDistance");
    p.attackPlayer = filthAttackOrdinal(obj->scalar("attack_player"));
    p.initialGeoprocess = scalarBool(obj, "initial_geoprocess");
    p.createFirst = scalarBool(obj, "create_first");
    return true;
}


} // namespace

// ---------------------------------------------------------------------------
// ScumSpots members
// ---------------------------------------------------------------------------

std::vector<SpgFile::ScumSpot> SpgFile::scumSpots() const
{
    std::vector<ScumSpot> out;
    if (!loaded()) {
        return out;
    }
    XPrmObject* bArray = buildingsArray(save_, false);
    XPrmObject* fArray = filthObjectsArray(save_, false);

    std::vector<ParsedFilth> filths;
    if (fArray) {
        for (std::size_t i = 1; i < fArray->fields.size(); ++i) {
            ParsedFilth parsed;
            if (parseFilthEntry(fArray, i, parsed)) {
                filths.push_back(std::move(parsed));
            }
        }
    }

    if (!bArray) {
        return out;
    }
    for (std::size_t i = 1; i < bArray->fields.size(); ++i) {
        const XPrmObject::Field& entry = bArray->fields[i];
        if (!entry.hasObject || !entry.object) {
            continue;
        }
        const XPrmObject* obj = entry.object.get();
        const ScumSpotInfo* info = nullptr;
        const std::string attrText = obj->scalar("attributeID");
        for (const ScumSpotInfo& candidate : kScumSpots) {
            if (attrText == candidate.identifier) {
                info = &candidate;
                break;
            }
        }
        if (!info) {
            continue; // not a scum mound — other code's business
        }
        ScumSpot s;
        s.index = out.size();
        s.buildingIndex = static_cast<int>(i) - 1;
        s.attributeId = info->id;
        s.unitId = scalarInt(obj, "unitID");
        const XPrmObject* pos = obj->object("position");
        s.x = scalarDouble(pos, "x");
        s.y = scalarDouble(pos, "y");
        s.z = scalarDouble(pos, "z");
        const XPrmObject* quat = obj->object("orientaion");
        const double qs = scalarDouble(quat, "s");
        const double qz = scalarDouble(quat, "z");
        double angle = 2.0 * std::atan2(qz, qs) * 180.0 /
                       3.14159265358979323846;
        while (angle > 180.0) angle -= 360.0;
        while (angle <= -180.0) angle += 360.0;
        s.angleDeg = angle;
        int deadSum = 0;
        int maxSum = 0;
        for (int k = 0; k < 5; ++k) {
            maxSum += info->molecula[static_cast<std::size_t>(k)];
        }
        const XPrmObject* damage = obj->object("damageMolecula");
        const XPrmObject* elements =
            damage ? damage->object("elementsDead") : nullptr;
        if (damage && damage->scalar("isAlive") == "false") {
            s.hp = 0;
        } else if (elements && elements->fields.size() >= 6) {
            for (std::size_t k = 1; k <= 5; ++k) {
                deadSum += std::atoi(elements->fields[k].scalar.c_str());
            }
            s.hp = maxSum > 0
                       ? std::clamp(static_cast<int>(std::llround(
                             100.0 * (1.0 - static_cast<double>(deadSum) /
                                               maxSum))),
                                    0, 100)
                       : 100;
        }
        // Pair with the nearest spawner under the mound.
        double best = kScumPairDistance;
        int bestIdx = -1;
        for (std::size_t k = 0; k < filths.size(); ++k) {
            const double d =
                std::hypot(filths[k].x - s.x, filths[k].y - s.y);
            if (d <= best) {
                best = d;
                bestIdx = static_cast<int>(k);
            }
        }
        if (bestIdx >= 0) {
            s.paired = true;
            s.filthIndex = filths[static_cast<std::size_t>(bestIdx)].fieldIndex;
            s.filth = filths[static_cast<std::size_t>(bestIdx)].params;
        }
        // The nest identity: the MOUND's own label when it carries one
        // (editor nests — the trigger wiring watches the mound by it);
        // otherwise fall back to the spawner label for display only.
        const std::string moundLabel = unquoted(obj->scalar("label"));
        s.moundLabeled = !moundLabel.empty();
        if (s.moundLabeled) {
            s.label = moundLabel;
        } else if (bestIdx >= 0) {
            s.label = filths[static_cast<std::size_t>(bestIdx)].label;
        }
        out.push_back(std::move(s));
    }
    return out;
}

bool SpgFile::addScumSpot(ScumSpot& spot)
{
    if (!loaded()) {
        return false;
    }
    const ScumSpotInfo* info = scumSpotInfo(spot.attributeId);
    XPrmObject* bArray = buildingsArray(save_, true);
    XPrmObject* fArray = filthObjectsArray(save_, true);
    if (!info || !bArray || !fArray) {
        return false;
    }
    const int moundId = maxUnitId() + 1;
    spot.unitId = moundId;
    spot.label = "scum_spot_" + std::to_string(moundId);
    spot.paired = true;
    spot.moundLabeled = true;

    XPrmObject::Field mound;
    mound.typeTag = "struct SaveUnitBuildingData";
    mound.hasObject = true;
    mound.object = scumBuildingObject(spot, *info, spot.label);
    mound.edited = true;
    appendArrayElement(bArray, std::move(mound));
    spot.buildingIndex = static_cast<int>(bArray->elementCount()) - 1;

    // The spawner sits at the same x/y ON the zero layer (03ISOLAND
    // pattern: z = zeroLayerHeight). Its label is the nest label + "_gen" —
    // the trigger ACTIONS target it while the CONDITIONS watch the mound.
    XPrmObject::Field spawner;
    spawner.typeTag = "struct SaveUnitFilthData";
    spawner.hasObject = true;
    spawner.object = filthEntryObject(moundId + 1, spot.x, spot.y,
                                      zeroLayerHeight(),
                                      spot.label + "_gen", spot.filth);
    spawner.edited = true;
    appendArrayElement(fArray, std::move(spawner));
    spot.filthIndex = static_cast<int>(fArray->elementCount()) - 1;
    return true;
}

bool SpgFile::setScumSpotPose(std::size_t index, double x, double y,
                              double angleDeg)
{
    if (!loaded()) {
        return false;
    }
    const std::vector<ScumSpot> spots = scumSpots();
    if (index >= spots.size()) {
        return false;
    }
    const ScumSpot& s = spots[index];
    XPrmObject* bArray = buildingsArray(save_, false);
    if (!bArray || s.buildingIndex < 0 ||
        static_cast<std::size_t>(s.buildingIndex) + 1 >=
            bArray->fields.size()) {
        return false;
    }
    poseBuildingObject(bArray->fields[static_cast<std::size_t>(s.buildingIndex) + 1]
                           .object.get(),
                       x, y, angleDeg);
    if (s.paired) {
        XPrmObject* fArray = filthObjectsArray(save_, false);
        if (fArray && s.filthIndex >= 0 &&
            static_cast<std::size_t>(s.filthIndex) + 1 <
                fArray->fields.size()) {
            XPrmObject* pos =
                fArray->fields[static_cast<std::size_t>(s.filthIndex) + 1]
                    .object->object("position");
            if (pos) {
                pos->setScalar("x", formatXPrmNumber(x));
                pos->setScalar("y", formatXPrmNumber(y));
            }
        }
    }
    return true;
}

bool SpgFile::setScumSpotHp(std::size_t index, int hp)
{
    if (!loaded()) {
        return false;
    }
    const std::vector<ScumSpot> spots = scumSpots();
    if (index >= spots.size()) {
        return false;
    }
    const ScumSpot& s = spots[index];
    const ScumSpotInfo* info = scumSpotInfo(s.attributeId);
    XPrmObject* bArray = buildingsArray(save_, false);
    if (!info || !bArray || s.buildingIndex < 0 ||
        static_cast<std::size_t>(s.buildingIndex) + 1 >=
            bArray->fields.size()) {
        return false;
    }
    XPrmObject* obj =
        bArray->fields[static_cast<std::size_t>(s.buildingIndex) + 1]
            .object.get();
    XPrmObject::Field* damageField = obj->field("damageMolecula");
    if (!damageField) {
        return false;
    }
    damageField->hasObject = true;
    damageField->object = damageObject(hp, info->molecula);
    damageField->edited = true;
    return true;
}

bool SpgFile::setScumSpotFilth(std::size_t index, const FilthParams& filth)
{
    if (!loaded()) {
        return false;
    }
    const std::vector<ScumSpot> spots = scumSpots();
    if (index >= spots.size()) {
        return false;
    }
    const ScumSpot& s = spots[index];
    if (!s.paired || s.filthIndex < 0) {
        return false;
    }
    XPrmObject* fArray = filthObjectsArray(save_, false);
    if (!fArray || static_cast<std::size_t>(s.filthIndex) + 1 >=
                        fArray->fields.size()) {
        return false;
    }
    XPrmObject::Field& entry =
        fArray->fields[static_cast<std::size_t>(s.filthIndex) + 1];
    XPrmObject* old = entry.object.get();
    const int unitId = scalarInt(old, "unitID");
    const XPrmObject* pos = old->object("position");
    const double z = scalarDouble(pos, "z");
    const std::string label = unquoted(old->scalar("label"));
    entry.object = filthEntryObject(unitId, s.x, s.y, z, label, filth);
    entry.edited = true;
    return true;
}

bool SpgFile::removeScumSpot(std::size_t index)
{
    if (!loaded()) {
        return false;
    }
    const std::vector<ScumSpot> spots = scumSpots();
    if (index >= spots.size()) {
        return false;
    }
    const ScumSpot& s = spots[index];
    XPrmObject* bArray = buildingsArray(save_, false);
    if (bArray && s.buildingIndex >= 0) {
        removeArrayElement(bArray, s.buildingIndex);
    }
    if (s.paired && s.filthIndex >= 0) {
        XPrmObject* fArray = filthObjectsArray(save_, false);
        if (fArray) {
            removeArrayElement(fArray, s.filthIndex);
        }
    }
    return true;
}

bool SpgFile::syncScumTriggerChain(const std::string& chainFile, bool enable)
{
    if (!loaded()) {
        return false;
    }
    XPrmObject* manual = save_->object("manualData");
    XPrmObject* players = manual ? manual->object("players") : nullptr;
    if (!players) {
        return false;
    }
    for (std::size_t i = 1; i < players->fields.size(); ++i) {
        if (!players->fields[i].hasObject || !players->fields[i].object) {
            continue;
        }
        XPrmObject* player = players->fields[i].object.get();

        // The modern list: TriggerChainNames = { N; { name = "x"; }, ... }.
        XPrmObject* tcn = player->object("TriggerChainNames");
        int foundRow = -1;
        if (tcn) {
            for (std::size_t j = 1; j < tcn->fields.size(); ++j) {
                const XPrmObject::Field& e = tcn->fields[j];
                if (e.hasObject && e.object &&
                    unquoted(e.object->scalar("name")) == chainFile) {
                    foundRow = static_cast<int>(j) - 1;
                    break;
                }
            }
        }
        if (enable) {
            if (foundRow < 0) {
                if (!tcn) {
                    auto empty = std::make_shared<XPrmObject>();
                    empty->fields.push_back(anonScalar("0", ';'));
                    empty->gaps.assign(2, " ");
                    XPrmObject::Field field;
                    field.name = "TriggerChainNames";
                    field.hasObject = true;
                    field.object = empty;
                    field.terminator = ';';
                    field.edited = true;
                    player->addField(std::move(field));
                    tcn = player->object("TriggerChainNames");
                }
                XPrmObject::Field entry;
                entry.hasObject = true;
                entry.object =
                    freshObject({namedScalar("name", quoted(chainFile))});
                entry.edited = true;
                appendArrayElement(tcn, std::move(entry));
            }
        } else if (foundRow >= 0) {
            removeArrayElement(tcn, foundRow);
        }

        // The legacy lowercase list merges into TriggerChainNames on read —
        // purge the name from it in both modes (it must appear exactly once,
        // in the modern list).
        XPrmObject* legacy = player->object("triggerChainNames");
        if (legacy) {
            for (std::size_t j = 1; j < legacy->fields.size();) {
                const XPrmObject::Field& e = legacy->fields[j];
                if (!e.hasObject && unquoted(e.scalar) == chainFile) {
                    removeArrayElement(legacy, static_cast<int>(j) - 1);
                } else {
                    ++j;
                }
            }
        }
    }
    return true;
}

bool SpgFile::normalizeScumNestLabels()
{
    if (!loaded()) {
        return false;
    }
    bool changed = false;
    const std::vector<ScumSpot> spots = scumSpots();
    for (const ScumSpot& s : spots) {
        if (!s.paired || s.filthIndex < 0 || s.moundLabeled) {
            continue;
        }
        // Pre-fix nests carry the nest label on the SPAWNER only — move it
        // to the mound and give the spawner the "_gen" action-target label.
        // Mounds with any other (foreign) label scheme are left alone.
        XPrmObject* fArray = filthObjectsArray(save_, false);
        XPrmObject* bArray = buildingsArray(save_, false);
        if (!fArray || !bArray || s.filthIndex + 1 >= static_cast<int>(fArray->fields.size()) ||
            s.buildingIndex + 1 >= static_cast<int>(bArray->fields.size())) {
            continue;
        }
        XPrmObject* filth =
            fArray->fields[static_cast<std::size_t>(s.filthIndex) + 1]
                .object.get();
        const std::string filthLabel = unquoted(filth->scalar("label"));
        if (filthLabel.rfind("scum_spot_", 0) != 0 ||
            filthLabel.size() < 5 ||
            filthLabel.compare(filthLabel.size() - 4, 4, "_gen") == 0) {
            continue;
        }
        filth->setScalar("label", quoted(filthLabel + "_gen"));
        XPrmObject* mound =
            bArray->fields[static_cast<std::size_t>(s.buildingIndex) + 1]
                .object.get();
        mound->setScalar("label", quoted(filthLabel));
        changed = true;
    }
    return changed;
}

std::string SpgFile::buildScumTriggerScript(
    const std::string& chainName) const
{
    const std::vector<ScumSpot> spots = scumSpots();

    const char* triggerTail =
        "\n\t\t\toutcomingLinks = {\n"
        "\t\t\t\t0;\n"
        "\t\t\t};\n"
        "\t\t\tstate_ = SLEEPING;\n"
        "\t\t\texecutionCounter_ = 0;\n"
        "\t\t\tinternalColor_ = 0;\n"
        "\t\t\tcellIndex = {\n"
        "\t\t\t\tx = 0;\n"
        "\t\t\t\ty = 0;\n"
        "\t\t\t};\n"
        "\t\t\tboundingRect = {\n"
        "\t\t\t\tleft = 0;\n"
        "\t\t\t\ttop = 0;\n"
        "\t\t\t\tright = 0;\n"
        "\t\t\t\tbottom = 0;\n"
        "\t\t\t};\n"
        "\t\t}";

    std::vector<std::string> texts;

    // START: always-true condition, links wake every pair trigger.
    std::string start =
        "\t\t{\n"
        "\t\t\tname = \"START\";\n"
        "\t\t\tcondition = \"struct ConditionSwitcher\" {\n"
        "\t\t\t\tstate_ = true;\n"
        "\t\t\t\tinternalColor_ = 0;\n"
        "\t\t\t\ttype = AND;\n"
        "\t\t\t\tconditions = {\n"
        "\t\t\t\t\t1;\n"
        "\t\t\t\t\t{\n"
        "\t\t\t\t\t\ttype = NORMAL;\n"
        "\t\t\t\t\t\tcondition = \"struct ConditionSwitcher\" {\n"
        "\t\t\t\t\t\t\tstate_ = true;\n"
        "\t\t\t\t\t\t\tinternalColor_ = 0;\n"
        "\t\t\t\t\t\t\ttype = AND;\n"
        "\t\t\t\t\t\t\tconditions = {\n"
        "\t\t\t\t\t\t\t\t0;\n"
        "\t\t\t\t\t\t\t};\n"
        "\t\t\t\t\t\t};\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t};\n"
        "\t\t\t};\n"
        "\t\t\taction = 0;\n";
    {
        std::vector<std::string> links;
        for (const ScumSpot& s : spots) {
            if (!s.paired || !s.moundLabeled) {
                continue; // no mound label — nothing the wiring can watch
            }
            links.push_back(s.label + "_on");
            links.push_back(s.label + "_off");
        }
        std::string linkText =
            "\t\t\toutcomingLinks = {\n\t\t\t\t" +
            std::to_string(links.size()) + ";\n";
        for (std::size_t i = 0; i < links.size(); ++i) {
            linkText += "\t\t\t\t{\n"
                        "\t\t\t\t\ttriggerName = \"" +
                        links[i] +
                        "\";\n"
                        "\t\t\t\t\tcolor = STRATEGY_RED;\n"
                        "\t\t\t\t\ttype = THIN;\n"
                        "\t\t\t\t\tactive_ = false;\n"
                        "\t\t\t\t\tparentOffset = {\n"
                        "\t\t\t\t\t\tx = 0;\n"
                        "\t\t\t\t\t\ty = 0;\n"
                        "\t\t\t\t\t};\n"
                        "\t\t\t\t\tchildOffset = {\n"
                        "\t\t\t\t\t\tx = 0;\n"
                        "\t\t\t\t\t\ty = 0;\n"
                        "\t\t\t\t\t};\n"
                        "\t\t\t\t}" +
                        (i + 1 < links.size() ? "," : "") + "\n";
        }
        linkText += "\t\t\t};\n";
        start += linkText;
    }
    // START must be stored CHECKING: TriggerChain::load -> initialize() ->
    // buildLinks() seeds activeTriggers_ from the STORED state
    // (the SLEEPING->CHECKING reset in initializeTriggersAndLinks runs only
    // in the debug editor path). A SLEEPING START leaves the whole chain
    // dead — exactly how the shipped chains store it (03isoland.scr).
    start += "\t\t\tstate_ = CHECKING;\n"
            "\t\t\texecutionCounter_ = 0;\n"
            "\t\t\tinternalColor_ = 0;\n"
            "\t\t\tcellIndex = {\n"
            "\t\t\t\tx = 0;\n"
            "\t\t\t\ty = 0;\n"
            "\t\t\t};\n"
            "\t\t\tboundingRect = {\n"
            "\t\t\t\tleft = 0;\n"
            "\t\t\t\ttop = 0;\n"
            "\t\t\t\tright = 0;\n"
            "\t\t\t\tbottom = 0;\n"
            "\t\t\t};\n"
            "\t\t}";
    texts.push_back(std::move(start));

    // Per pair: the conditions watch the MOUND by its label — exists while
    // it stands (activates the spawner), a destroy event once it dies
    // (deactivates it). Distance-based wiring is impossible here: the game
    // loads spawner and mound at the SAME coordinates and
    // terPlayer::findUnit(id, pos) filters distanceMin=0 with `dist > 0`,
    // excluding a co-located unit outright (the live EXTDEMO bug).
    for (const ScumSpot& s : spots) {
        if (!s.paired || !s.moundLabeled) {
            continue;
        }
        const std::string genLabel = s.label + "_gen";
        texts.push_back(
            "\t\t{\n"
            "\t\t\tname = \"" + s.label + "_on\";\n"
            "\t\t\tcondition = \"struct ConditionObjectByLabelExists\" {\n"
            "\t\t\t\tstate_ = false;\n"
            "\t\t\t\tinternalColor_ = 0;\n"
            "\t\t\t\tlabel = \"" + s.label + "\";\n"
            "\t\t\t};\n"
            "\t\t\taction = \"struct ActionActivateObjectByLabel\" {\n"
            "\t\t\t\tinternalColor_ = 0;\n"
            "\t\t\t\tlabel = \"" + genLabel + "\";\n"
            "\t\t\t};\n" + triggerTail);

        texts.push_back(
            "\t\t{\n"
            "\t\t\tname = \"" + s.label + "_off\";\n"
            "\t\t\tcondition = \"struct ConditionKillObjectByLabel\" {\n"
            "\t\t\t\tstate_ = false;\n"
            "\t\t\t\tinternalColor_ = 0;\n"
            "\t\t\t\tlabel = \"" + s.label + "\";\n"
            "\t\t\t};\n"
            "\t\t\taction = \"struct ActionDeactivateObjectByLabel\" {\n"
            "\t\t\t\tinternalColor_ = 0;\n"
            "\t\t\t\tlabel = \"" + genLabel + "\";\n"
            "\t\t\t};\n" + triggerTail);
    }

    std::string out;
    out += "TriggerChain = {\n";
    out += "\tname = \"" + chainName + "\";\n";
    out += "\ttriggers = {\n";
    out += "\t\t" + std::to_string(texts.size()) + ";\n";
    for (std::size_t i = 0; i < texts.size(); ++i) {
        out += texts[i];
        out += (i + 1 < texts.size() ? ",\n" : "\n");
    }
    out += "\t};\n";
    out += "\tboundingRect = {\n"
           "\t\tleft = 0;\n"
           "\t\ttop = 0;\n"
           "\t\tright = 0;\n"
           "\t\tbottom = 0;\n"
           "\t};\n";
    out += "\tviewRect = {\n"
           "\t\tleft = 0;\n"
           "\t\ttop = 0;\n"
           "\t\tright = 0;\n"
           "\t\tbottom = 0;\n"
           "\t};\n";
    out += "};\n";
    return out;
}

int SpgFile::filthTypeCount()
{
    return kFilthTypeCount;
}

const char* SpgFile::filthTypeIdentifier(int ordinal)
{
    return filthTypeName(ordinal);
}

} // namespace MapEditor
