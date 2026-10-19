#pragma once

#include <string>
#include <vector>

// ============================================================================
// Preflight validation for the map editor.
// Before any Terra call the editor must verify that the game/world directories
// exist and contain all mandatory files.
// ============================================================================

namespace MapEditor {

/// Result of a preflight check.
struct PreflightResult
{
    bool ok = true;
    std::vector<std::string> errors;    // non-empty ⇒ fatal
    std::vector<std::string> warnings;  // non-empty ⇒ non-fatal
    std::vector<std::string> info;      // informational messages
};

/// Check that a game directory is usable for the map editor.
///
/// Verifies:
///  - Directory exists
///  - RESOURCE/Worlds/ exists and contains at least one world folder
///  - The first world contains at least output.vmp and world.ini
///
/// Returns a result struct; call PreflightResult::ok to check success.
PreflightResult preflightCheck(const std::string& gameDir);

/// Print the PreflightResult to stdout/stderr.
void printPreflightResult(const PreflightResult& result, const std::string& gameDir);

} // namespace MapEditor

