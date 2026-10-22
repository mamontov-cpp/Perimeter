// ============================================================================
// MapEditorCore — PaletteOps implementation (T4.1). See PaletteOps.h.
// ============================================================================

#include "PaletteOps.h"

#include <cstdio>

namespace MapEditor {

std::string PaletteOps::geoPalXml(const unsigned char* geoPalRgb)
{
    if (!geoPalRgb) {
        return std::string();
    }

    std::string xml;
    // Each line is ~50 bytes; reserve once for all 256 entries so the
    // document stays well under vrtMap::loadGeoDamPal's read buffer
    // (258*60 bytes — VMAP.cpp:1412).
    xml.reserve(40 + PaletteEntryCount * 50);

    xml += "<colorTable>\n";
    for (int i = 0; i < PaletteEntryCount; ++i) {
        xml += "\t<color value='";
        for (int c = 0; c < 3; ++c) {
            const unsigned char byte = geoPalRgb[i * 3 + c];
            char text[16];
            std::snprintf(text, sizeof(text), "%f", (byte + 0.5) / 255.0);
            if (c != 0) {
                xml += ',';
            }
            xml += text;
        }
        xml += "'/>\n";
    }
    xml += "</colorTable>\n";
    return xml;
}

} // namespace MapEditor
