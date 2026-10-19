// Placeholder translation unit that keeps the MapEditorCore static library valid
// until the real core sources (EditorSession, WorldFiles, BrushTool, ...) are added in T1+.
namespace MapEditor {

// Returns the editor core ABI marker, used only to keep this translation unit non-empty.
int coreAbiMarker()
{
    return 1;
}

} // namespace MapEditor
