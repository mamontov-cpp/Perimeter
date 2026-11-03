#pragma once

#include <string>

// ============================================================================
// MapEditorCore — the scripted brush (JavaScript via duktape).
//
// BrushEffect::Script runs params.script once per stamp at the stamp
// centre. The script calls the editor brush API (height/zeroplast/soot/
// very-light-dam/palette ops at explicit positions and radii), reads the
// current brush parameters via getCurrent*(), and is bounded by a time
// limit enforced through the duktape interrupt mechanism. The whole
// evaluation lands in ONE undo entry (EditorSession undo group).
// ============================================================================

namespace MapEditor {

class BrushTool;
class EditorSession;
struct BrushParams;

/// Compiles the script without running it — the script editor's OK-button
/// syntax check. Returns false and fills errorOut with the duktape error
/// (which carries the "brush.js" line number).
bool checkScriptSyntax(const std::string& script, std::string& errorOut);

/// Runs the scripted brush at the stamp centre (cx, cy). Dam mode only.
/// Every world edit the script performs is collected into ONE undo entry;
/// a failing script keeps its partial edits under that same entry.
/// `timeoutMs` bounds the evaluation (duktape interrupt, default 20 s).
/// Returns false and fills errorOut on any failure: empty script, wrong
/// mode, syntax error, runtime error (message includes brush.js:line), or
/// timeout.
bool runScriptedBrush(EditorSession& session, BrushTool& brush,
                      const BrushParams& params, int cx, int cy,
                      std::string& errorOut, int timeoutMs = 20000);

/// UI hook for the scripted brush's showInfoMessageBox/showErrorMessageBox:
/// the core is Qt-free, so the UI layer installs this callback and the core
/// forwards every message to it (isError selects the info/error mode).
/// The hook may be null — messages are then dropped silently.
void setScriptedBrushMessageHook(void (*hook)(bool isError,
                                             const std::string& text));

} // namespace MapEditor
