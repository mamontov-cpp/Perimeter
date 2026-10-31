#pragma once

#include <string>
#include <vector>

// ============================================================================
// MapEditorCore — .spg user-text encoding (T8 addendum).
//
// The game writes the editable strings of a battle map (the player names)
// in its system codepage — Windows-1251 for the Russian release, plain
// ASCII otherwise. The XPrmDocument stores raw file bytes; SpgFile
// transcodes at its boundary so that the rest of the editor (UI, tests,
// validation) works in UTF-8 only:
//
//   load:  detect the encoding from the player names, remember it;
//   read:  spgTextToUtf8(file bytes)  -> QString::fromStdString just works;
//   write: utf8ToSpgText(UTF-8) -> file bytes in the SAME encoding, so an
//          unmodified name survives a load/save round-trip byte-exactly and
//          an edited name stays readable by the game.
//
// ASCII is identical in both encodings and validates as UTF-8, so English
// maps need no special handling.
// ============================================================================

namespace MapEditor {

/// Text encoding of the user-editable strings (player names) in a .spg file.
enum class SpgTextEncoding
{
    Utf8,
    Windows1251,
};

/// Decide the encoding from sample strings (the player names): UTF-8 unless
/// some sample carries non-ASCII bytes that do not form strict valid UTF-8
/// (no overlongs, no surrogates, capped at U+10FFFF) — that is the legacy
/// Windows-1251 case.
SpgTextEncoding detectSpgTextEncoding(const std::vector<std::string>& samples);

/// File bytes -> UTF-8 (for the UI and internal comparisons).
std::string spgTextToUtf8(const std::string& bytes, SpgTextEncoding encoding);

/// UTF-8 -> file bytes; characters unrepresentable in the target encoding
/// degrade to '?' — the same lossy behaviour the game itself shows for them.
std::string utf8ToSpgText(const std::string& utf8, SpgTextEncoding encoding);

} // namespace MapEditor
