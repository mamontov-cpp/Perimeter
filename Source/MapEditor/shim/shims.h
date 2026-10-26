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
// INI helpers (ReadIniString/WriteIniString/IniManager) — provided by the real
// Util/SystemUtil since the GPU viewport work: the render stack pulls
// SystemUtil.obj into the editor link, and duplicate definitions (this shim's
// older clones) would break it. Include "SystemUtil.h" instead.
// ---------------------------------------------------------------------------

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
