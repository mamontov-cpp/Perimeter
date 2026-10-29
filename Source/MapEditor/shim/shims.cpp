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
// terEnergyBorderCheck  →  1 (always succeeds)
//
// The game's semantics (Game/Player.cpp:1213): returns 1 when the cell is
// NOT inside any player's energy column — the game refuses to erase the
// zeroplast mark under one (Terra's T2TE_ERASE_ZEROPLAST, tools.h:525).
// The editor has no energy column at all, so "outside the border" is always
// true; returning 0 would mean "inside the border" and silently turns every
// consumer of this check into a no-op (it once kept an erase toolzer dead,
// which is why the brush existed but did nothing). No current editor path
// calls the check, but the shims must state the correct empty-world
// contract for any future consumer.
// ---------------------------------------------------------------------------

int terEnergyBorderCheck(int /*x*/, int /*y*/)
{
    return 1;
}

// ---------------------------------------------------------------------------
// currentShortVersion
// ---------------------------------------------------------------------------

const char* currentShortVersion = PERIMETER_VERSION;
