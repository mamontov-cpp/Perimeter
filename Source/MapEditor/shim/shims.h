#pragma once

#include <cstdint>
#include <string>

// ============================================================================
// MapEditorSupport shims — minimal replacements for game-only symbols.
// These allow MapEditorCore (and the editor itself) to link against Terra /
// XTool without pulling in SDL2 or game subsystems.
// ============================================================================

// ---------------------------------------------------------------------------
// RandomGenerator / logicRND  (from XTool/xmath.h + Util/DebugUtil.cpp)
// ---------------------------------------------------------------------------
class RandomGenerator;                          // forward from XTool/xmath.h

extern RandomGenerator logicRND;                // global instance, seed=12345678

int  logicRNDi_internal(int x, const char* file, int line);
float logicRNDf_internal(const char* file, int line);
float logicRNDfa_internal(const char* file, int line);

// ---------------------------------------------------------------------------
// INI helpers  (from Util/SystemUtil.cpp + SimpleIni)
// ---------------------------------------------------------------------------
uint32_t ReadIniString(const char* section, const char* key, const char* defaultVal,
                       char* returnBuffer, uint32_t bufferSize, const std::string& filePath);

uint32_t WriteIniString(const char* section, const char* key, const char* value,
                        const std::string& filePath);

// ---------------------------------------------------------------------------
// IniManager  (from Util/SystemUtil.cpp)
// ---------------------------------------------------------------------------
class IniManager
{
private:
    void* ini_instance = nullptr;
    bool pending_changes = false;
    std::string fname_;
    bool is_full_path;
    std::string getFilePath();
public:
    bool check_existence = true;
    explicit IniManager(const char* fname, bool check_existence = true, bool full_path = false);
    ~IniManager();
    bool load();
    bool save();
    const char* get(const char* section, const char* key);
    void put(const char* section, const char* key, const char* val);
    int getInt(const char* section, const char* key);
    bool getInt(const char* section, const char* key, int& value);
    void putInt(const char* section, const char* key, int val);
    float getFloat(const char* section, const char* key);
    bool getFloat(const char* section, const char* key, float& value);
    void putFloat(const char* section, const char* key, float val);
    void getFloatArray(const char* section, const char* key, int size, float array[]);
    void putFloatArray(const char* section, const char* key, int size, const float array[]);
};

// ---------------------------------------------------------------------------
// terEnergyBorderCheck  (from Game/Player.cpp)
// ---------------------------------------------------------------------------
int terEnergyBorderCheck(int x, int y);

// ---------------------------------------------------------------------------
// currentShortVersion  (from Game/Runtime.cpp)
// ---------------------------------------------------------------------------
extern const char* currentShortVersion;

// ---------------------------------------------------------------------------
// content-root helpers  (from Game/GameContent.cpp + XTool/files/files.h)
// ---------------------------------------------------------------------------
void init_content_root();
