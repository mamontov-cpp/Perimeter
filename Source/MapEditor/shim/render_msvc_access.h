// ============================================================================
// MapEditorRender — MSVC member-access widening (applied via /FI forced
// include on every TU of the target, see CMakeLists.txt).
//
// MSVC encodes the ACCESS of a member function (private 'A', protected 'P',
// public 'Q') in its mangled name. The editor device subclass
// (render/EditorSokolDevice.cpp) must call private/protected cSokolRender
// members, widened there by a '#define private public' shim; when the sokol
// render TUs compiled those same members as private the linker saw two
// different symbols and resolved nothing. Every MapEditorRender TU therefore
// compiles with access widened, so all members mangle as public and the
// references agree.
//
// This only affects symbol mangling inside MapEditorRender: the target
// REPLACES the game's Render/RenderSokol libraries in the editor link (they
// are not linked), and no other module references these classes' non-public
// members. The code is compiled unmodified apart from access visibility.
// ============================================================================

// The MSVC STL's xkeycheck.h #errors on keyword macroization; this sanctioned
// escape hatch disables that check for these TUs only.
#define _ALLOW_KEYWORD_MACROS

#pragma push_macro("private")
#pragma push_macro("protected")
#undef private
#undef protected
#define private public
#define protected public
