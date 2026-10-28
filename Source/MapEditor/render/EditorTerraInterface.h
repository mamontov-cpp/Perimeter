// ============================================================================
// MapEditorRender — TerraInterface feeding the game's tilemap from the
// editor's world state.
// ============================================================================

#pragma once

#include "StdAfxRD.h"

// World state (vMap / H_SIZE / hZeroPlast) — StandartTerraInterface touches
// the vrtMap directly; the game TUs get it the same way. Must come before
// TerraInterface.inl (vMap / VX_FRACTION / kmGrid are used inline there).
#include "terra.h"

#include "../inc/IncTerra.h"
#include "../inc/TerraInterface.inl"

// Full type: GetTileColor samples the editor's color cache inline.
#include "ViewportRenderer.h"

class EditorTerraInterface : public StandartTerraInterface {
public:
    /// `colors` — not owned; the base raster the 2D view shows. When bound,
    /// tile colors come from it, so the GPU view honours the same layer
    /// filter / geo-dimmed state as the TopDown cache. Otherwise raw
    /// vrtMap::getColor32() values are used (the game's own source).
    explicit EditorTerraInterface(MapEditor::ViewportRenderer* colors)
        : colors_(colors)
    {
    }

    /// Editor variant of PerimeterTerraInterface::GetTileColor
    /// (Source/Game/CopyToGraph.cpp): clamped cell colors from the editor's
    /// 2D raster cache (layer filters + geo dimming included, exactly what
    /// the TopDown view shows). Unlike the game, the alpha is always opaque:
    /// the game marks zeroplast cells with alpha 254 so its own zeroplast
    /// quads can cover them, but this view is created with zeroplastnumber=0
    /// (the game's green zeroplast overlay is a runtime effect the CPU
    /// editor view does not show either) — a 254 marker would punch
    /// alpha-tested holes into the terrain instead.
    void GetTileColor(uint8_t* tile, uint32_t line_size,
                      int xstart, int ystart, int xend, int yend, int step) override
    {
        sColor4c color;
        for (int y = ystart; y < yend; y += step) {
            uint32_t* tx = reinterpret_cast<uint32_t*>(tile);
            int yy = min(max(0, y), vMap.clip_mask_y);
            for (int x = xstart; x < xend; x += step) {
                int xx = min(max(0, x), vMap.clip_mask_x);
                color.v = colors_ ? colors_->colorAt(xx, yy)
                                  : vMap.getColor32(xx, yy);
                color.v |= 0xFF000000;
                *tx = gb_RenderDevice->ConvertColor(color);
                tx++;
            }
            tile += line_size;
        }
    }

private:
    MapEditor::ViewportRenderer* colors_;
};
