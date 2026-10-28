// ============================================================================
// MapEditorSupport shims implementation.
// Replaces game-only symbols with minimal working implementations.
// ============================================================================

#include "shims.h"

#include <fstream>
#include <string>
#include <cstdio>

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
