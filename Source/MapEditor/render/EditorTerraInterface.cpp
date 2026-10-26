// ============================================================================
// MapEditorRender — TerraInterface implementation unit.
//
// Everything lives in the header (a single small class); this TU exists so
// the CMake target has a source and so the editor headers that pull the
// game render stack stay out of the Qt TUs.
// ============================================================================

#include "EditorTerraInterface.h"

#include "terra.h"

#include "ViewportRenderer.h"
