// ============================================================================
// MapEditorSupport shims implementation.
// Replaces game-only symbols with minimal working implementations.
// ============================================================================

#include "shims.h"

#include <fstream>
#include <string>
#include <sstream>
#include <cstdio>

// SimpleIni — included via CMake include-directories (${simpleini_SOURCE_DIR})
#define SI_SUPPORT_IOSTREAMS 1
#include <SimpleIni.h>

// RandomGenerator — from XTool/xmath.h
#include "xmath.h"

// Content root helpers — from XTool/files/files.h
#include "files/files.h"

// ---------------------------------------------------------------------------
// RandomGenerator / logicRND
// ---------------------------------------------------------------------------

// Global instance: same seed and logic flag as in DebugUtil.cpp
RandomGenerator logicRND(12345678, true);

int logicRNDi_internal(int x, const char* /*file*/, int /*line*/)
{
    return logicRND(x);
}

float logicRNDf_internal(const char* /*file*/, int /*line*/)
{
    return logicRND.frnd();
}

float logicRNDfa_internal(const char* /*file*/, int /*line*/)
{
    return logicRND.frand();
}

// ---------------------------------------------------------------------------
// ReadIniString / WriteIniString
// ---------------------------------------------------------------------------

uint32_t ReadIniString(const char* section, const char* key, const char* defaultVal,
                       char* returnBuffer, uint32_t bufferSize, const std::string& filePath)
{
    std::ifstream istream(filePath, std::ios::in | std::ios::binary);
    CSimpleIniA ini;
    SI_Error rc = ini.LoadData(istream);
    if (rc < 0) {
        fprintf(stderr, "ReadIniString: Error reading %s file: %d\n", filePath.c_str(), rc);
        return 0;
    }
    istream.close();

    const char* val = ini.GetValue(section, key, defaultVal);
    if (val) {
        strncpy(returnBuffer, val, bufferSize - 1);
        returnBuffer[bufferSize - 1] = 0;
    } else {
        *returnBuffer = 0;
    }

    ini.Reset();
    return 1;
}

uint32_t WriteIniString(const char* section, const char* key, const char* value,
                        const std::string& filePath)
{
    std::ifstream istream(filePath, std::ios::in | std::ios::binary);
    CSimpleIniA ini;
    SI_Error rc = ini.LoadData(istream);
    if (rc < 0) {
        fprintf(stderr, "WriteIniString: Error loading %s file for writing: %d\n",
                filePath.c_str(), rc);
        return 0;
    }
    istream.close();

    rc = ini.SetValue(section, key, value);
    if (rc < 0) {
        fprintf(stderr, "WriteIniString: Error writing [%s] %s = %s in %s: %d\n",
                section, key, value, filePath.c_str(), rc);
        return 0;
    }

    std::ofstream ostream(filePath, std::ios::out | std::ios::binary);
    rc = ini.Save(ostream);
    if (rc < 0) {
        fprintf(stderr, "WriteIniString: Error saving %s file: %d\n", filePath.c_str(), rc);
        return 0;
    }
    ostream.close();

    ini.Reset();
    return 1;
}

// ---------------------------------------------------------------------------
// IniManager
// ---------------------------------------------------------------------------

IniManager::IniManager(const char* fname, bool check_existence_, bool full_path)
    : fname_(fname), check_existence(check_existence_), is_full_path(full_path)
{
}

IniManager::~IniManager()
{
    CSimpleIniA* ini = static_cast<CSimpleIniA*>(ini_instance);
    if (ini) {
        save();
        delete ini;
    }
}

std::string IniManager::getFilePath()
{
    if (!is_full_path) {
        // Resolve relative path through the content root.
        return convert_path_content(fname_);
    }
    return fname_;
}

bool IniManager::load()
{
    CSimpleIniA* ini = static_cast<CSimpleIniA*>(ini_instance);
    if (!ini) {
        pending_changes = false;
        ini_instance = ini = new CSimpleIniA();
    }
    if (pending_changes) {
        save();
    }

    std::string path = getFilePath();
    std::ifstream istream(path, std::ios::in | std::ios::binary);
    SI_Error rc = ini->LoadData(istream);
    istream.close();
    if (rc < 0) {
        fprintf(stderr, "IniManager: Error reading %s file: %d\n", path.c_str(), rc);
        return false;
    }
    return true;
}

bool IniManager::save()
{
    if (!pending_changes) {
        return false;
    }
    pending_changes = false;

    std::string path = getFilePath();
    CSimpleIniA* ini = static_cast<CSimpleIniA*>(ini_instance);
    if (!ini) {
        fprintf(stderr, "IniManager: No ini loaded to write %s file\n", path.c_str());
        return false;
    }

    std::ofstream ostream(path, std::ios::out | std::ios::binary);
    SI_Error rc = ini->Save(ostream);
    ostream.close();
    if (rc < 0) {
        fprintf(stderr, "IniManager: Error writing %s file: %d\n", path.c_str(), rc);
        return false;
    }
    return true;
}

const char* IniManager::get(const char* section, const char* key)
{
    static const size_t BUF_SIZE = 256;
    static char buf[BUF_SIZE];
    *buf = 0;

    if (!ini_instance && !load()) {
        return buf;
    }

    CSimpleIniA* ini = static_cast<CSimpleIniA*>(ini_instance);
    const char* val = ini->GetValue(section, key, nullptr);
    if (val) {
        strncpy(buf, val, BUF_SIZE);
    } else if (check_existence) {
        std::string path = getFilePath();
        fprintf(stderr, "IniManager: key not found [%s] %s in %s\n", section, key, path.c_str());
    }

    return buf;
}

void IniManager::put(const char* section, const char* key, const char* val)
{
    if (!ini_instance && !load()) {
        return;
    }

    CSimpleIniA* ini = static_cast<CSimpleIniA*>(ini_instance);
    SI_Error rc = ini->SetValue(section, key, val);
    if (rc < 0) {
        std::string path = getFilePath();
        fprintf(stderr, "IniManager: Error writing [%s] %s = %s in %s: %d\n",
                section, key, val, path.c_str(), rc);
        return;
    }
    pending_changes = true;
}

int IniManager::getInt(const char* section, const char* key)
{
    int val = 0;
    getInt(section, key, val);
    return val;
}

bool IniManager::getInt(const char* section, const char* key, int& value)
{
    const char* str = get(section, key);
    if (*str) {
        value = static_cast<int>(strtol(str, nullptr, 0));
        return true;
    }
    return false;
}

void IniManager::putInt(const char* section, const char* key, int val)
{
    std::string str = std::to_string(val);
    put(section, key, str.c_str());
}

float IniManager::getFloat(const char* section, const char* key)
{
    float val = 0;
    getFloat(section, key, val);
    return val;
}

bool IniManager::getFloat(const char* section, const char* key, float& value)
{
    const char* str = get(section, key);
    if (*str) {
        value = static_cast<float>(strtof(str, nullptr));
        return true;
    }
    return false;
}

void IniManager::putFloat(const char* section, const char* key, float val)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%f", val);
    put(section, key, buf);
}

void IniManager::getFloatArray(const char* section, const char* key, int size, float array[])
{
    const char* str = get(section, key);
    // Parse space-separated floats using std::istringstream
    for (int i = 0; i < size; ++i) {
        array[i] = 0.0f;
    }
    std::istringstream stream(str);
    for (int i = 0; i < size; ++i) {
        float val = 0.0f;
        if (stream >> val) {
            array[i] = val;
        } else {
            break;
        }
    }
}

void IniManager::putFloatArray(const char* section, const char* key, int size, const float array[])
{
    std::string result;
    for (int i = 0; i < size; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%f ", array[i]);
        result += buf;
    }
    put(section, key, result.c_str());
}

// ---------------------------------------------------------------------------
// terEnergyBorderCheck  →  always returns 0 (no energy border)
// ---------------------------------------------------------------------------

int terEnergyBorderCheck(int /*x*/, int /*y*/)
{
    return 0;
}

// ---------------------------------------------------------------------------
// currentShortVersion
// ---------------------------------------------------------------------------

const char* currentShortVersion = PERIMETER_VERSION;
