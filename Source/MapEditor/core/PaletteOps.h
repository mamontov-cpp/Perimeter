#pragma once

#include <string>

// ============================================================================
// MapEditorCore — PaletteOps (T4.1).
//
// Palette utilities that do not need a loaded world, kept separate from
// EditorSession so they can be tested headlessly against the real game
// loaders:
//
//   * vrtMap::GeoPal / vrtMap::DamPal hold 256 palette entries of three bytes
//     each in plain RGB order (vrtMap::convertPal2TableSurCol reads palBuf
//     [i*3+0] as red — VMAP.cpp:1347).
//   * In _TX3D_LIBRARY_ builds the geo palette on disk lives in geoPal.xml
//     (<colorTable> of 256 <color value='r,g,b'/> tags, the same format the
//     shipped worlds carry); vrtMap::loadGeoDamPal parses it through
//     tx3d::IndexedTexture3D::extractColorTableFromXML and truncates
//     float*255 back into GeoPal bytes — so inGeo.act alone is NOT enough to
//     survive a reload, and editing the palette must sync the XML too.
// ============================================================================

namespace MapEditor {

class PaletteOps
{
public:
    /// Number of palette entries in vrtMap::GeoPal / vrtMap::DamPal (SIZE_GEO_
    /// PALETTE / 3; the terra header is deliberately not included here).
    static constexpr int PaletteEntryCount = 256;
    /// Total buffer size of one palette (SIZE_GEO_PALETTE).
    static constexpr int PaletteBytes = 768;

    /// Serialize a geo palette (PaletteBytes RGB bytes, the layout of
    /// vrtMap::GeoPal) as a geoPal.xml color table document:
    ///
    ///   <colorTable>
    ///   \t<color value='r,g,b'/>
    ///   ...
    ///   </colorTable>
    ///
    /// The float components are stored at (byte + 0.5)/255 instead of the
    /// exact byte/255: the game loader truncates float*255 back into bytes,
    /// and round-tripping byte/255 through printf/atof/float can land exactly
    /// on a truncation boundary and shift by one. The half-byte offset keeps
    /// every value in the middle of its truncation cell, so
    /// (unsigned char)(parse(serialize(b)) * 255) == b for all 256 bytes.
    /// Requires a buffer of PaletteBytes bytes; returns an empty document
    /// without entries when the pointer is null.
    static std::string geoPalXml(const unsigned char* geoPalRgb);
};

} // namespace MapEditor
