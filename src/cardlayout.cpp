#include "cardlayout.h"

#include <algorithm>

namespace cardlayout {

Result place(const Input& in) {
    auto btnFor = [&](float top) { return std::max(top + in.btnAnchor, top + in.stackH + in.gap); };
    Result r;
    r.infoY = in.infoY;
    r.btnY = btnFor(r.infoY);
    float bottom = in.btnH > 0 ? r.btnY + in.btnH : r.infoY + in.stackH;
    if (bottom > in.floorY) {
        // Up by the overrun, but not past the carousel.
        float up = std::min(bottom - in.floorY, std::max(0.0f, in.infoY - in.ceilY));
        r.infoY = in.infoY - up;
        r.btnY = btnFor(r.infoY);
    }
    return r;
}

}  // namespace cardlayout
