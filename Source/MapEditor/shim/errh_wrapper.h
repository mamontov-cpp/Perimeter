#pragma once

// ============================================================================
// MapEditorSupport — ErrH crash wrapper.
// Sets up ErrH.SetCrash() to perform an emergency save before termination.
// ============================================================================

namespace MapEditor {

/// Set up the ErrH crash handler so that on any crash the current world
/// buffers are saved back to disk before the program terminates.
///
/// Also configures ErrH with a prefix suitable for the editor.
void setupErrHWrapper();

} // namespace MapEditor
