#pragma once

#include <string>
#include <vector>

#include "cross_layout.h"

namespace thunderlink {

struct CrossLocalDisplay {
    cross::Display display;
    bool primary = false;
    float scale = 1.0f;
    int rotation = 0;
};

/* Enumerates the active Windows desktop topology. The caller owns placement of
 * remote tiles; these local bounds deliberately remain the OS desktop bounds. */
std::vector<CrossLocalDisplay> EnumerateCrossLocalDisplays();

/* Shows a short numbered overlay on every active Windows display. This has no
 * effect on the OS display arrangement and is safe to call from the UI thread. */
void IdentifyCrossLocalDisplays(const std::vector<CrossLocalDisplay>& displays);

} // namespace thunderlink
