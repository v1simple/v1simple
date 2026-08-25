#pragma once

constexpr bool displayFrameHasNothingToFlush(bool needsFullRedraw, bool drawnRegionEmpty) {
    return !needsFullRedraw && drawnRegionEmpty;
}
