#pragma once
// Platform-independent Cross canvas geometry. Coordinates are canvas units,
// not framebuffer pixels. The native adapters map within each display's bounds.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace thunderlink::cross {
struct Rect { double x = 0, y = 0, width = 0, height = 0; };
struct Display { std::string id, name; Rect bounds; bool remote = false; };
using Layout = std::vector<Display>;
struct Crossing { std::size_t target; double x, y; };
inline constexpr double EdgeEpsilon = 0.01;
inline constexpr double Inset = 0.5;
inline bool Near(double a, double b) { return std::abs(a - b) <= EdgeEpsilon; }
inline bool Contains(const Rect& r, double x, double y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}
inline std::pair<double, double> ClampTo(const Rect& r, double x, double y) {
    const double ix = std::min(Inset, r.width / 4), iy = std::min(Inset, r.height / 4);
    return {std::clamp(x, r.x + ix, r.x + r.width - ix),
            std::clamp(y, r.y + iy, r.y + r.height - iy)};
}
inline std::pair<double, double> Normalized(const Rect& r, double x, double y) {
    return {std::clamp((x-r.x)/r.width, 0.0, 1.0),
            std::clamp((y-r.y)/r.height, 0.0, 1.0)};
}
inline bool SharesEdge(const Rect& a, const Rect& b) {
    const double overlapY = std::min(a.y+a.height,b.y+b.height)-std::max(a.y,b.y);
    const double overlapX = std::min(a.x+a.width,b.x+b.width)-std::max(a.x,b.x);
    return ((Near(a.x+a.width,b.x)||Near(b.x+b.width,a.x)) && overlapY>EdgeEpsilon)
        || ((Near(a.y+a.height,b.y)||Near(b.y+b.height,a.y)) && overlapX>EdgeEpsilon);
}
inline std::string ValidateLayout(const Layout& layout, bool requireRemote = true) {
    if (layout.empty() || layout.size() > 64) return "Arrange between 1 and 64 displays.";
    std::unordered_set<std::string> ids;
    bool local = false, remote = false;
    for (const auto& d : layout) {
        const auto& r=d.bounds;
        if (d.id.empty() || d.id.size()>256 || !ids.insert(d.id).second)
            return "Display identities are missing or ambiguous. Identify the displays again.";
        if (!std::isfinite(r.x)||!std::isfinite(r.y)||!std::isfinite(r.width)||!std::isfinite(r.height)
            || r.width<1 || r.height<1 || r.width>1e6 || r.height>1e6 || std::abs(r.x)>1e7 || std::abs(r.y)>1e7)
            return "Display positions and sizes must be finite and positive.";
        local |= !d.remote; remote |= d.remote;
    }
    if (!local || (requireRemote && !remote)) return "Connect at least one Windows display and one Mac display.";
    for (std::size_t i=0;i<layout.size();++i) for (std::size_t j=i+1;j<layout.size();++j) {
        const auto& a=layout[i].bounds; const auto& b=layout[j].bounds;
        if (std::min(a.x+a.width,b.x+b.width)-std::max(a.x,b.x)>EdgeEpsilon &&
            std::min(a.y+a.height,b.y+b.height)-std::max(a.y,b.y)>EdgeEpsilon)
            return "Display tiles must not overlap.";
    }
    std::vector<bool> reachable(layout.size(),false);
    for (std::size_t i=0;i<layout.size();++i) reachable[i]=!layout[i].remote;
    for (std::size_t pass=0;pass<layout.size();++pass) {
        bool changed=false;
        for (std::size_t i=0;i<layout.size();++i) if (reachable[i])
            for (std::size_t j=0;j<layout.size();++j) if (!reachable[j] && SharesEdge(layout[i].bounds,layout[j].bounds)) {
                reachable[j]=true; changed=true;
            }
        if (!changed) break;
    }
    for (bool r:reachable) if (!r) return "Every Mac display needs a touching-edge route back to Windows.";
    return {};
}

// Find the first boundary hit by this movement, never tunnel through a gap.
// Return an inward landing point (not an extrapolated point past the new tile).
// Exact shared corners alone are not connections; upper endpoints are excluded.
inline std::optional<Crossing> CrossEdge(const Layout& layout,std::size_t current,
                                         double x,double y,double dx,double dy) {
    if (current>=layout.size() || !std::isfinite(x)||!std::isfinite(y)||!std::isfinite(dx)||!std::isfinite(dy)) return {};
    const auto& a=layout[current].bounds;
    if (a.width<=0 || a.height<=0) return {};
    if (x<a.x-EdgeEpsilon || x>a.x+a.width+EdgeEpsilon || y<a.y-EdgeEpsilon || y>a.y+a.height+EdgeEpsilon) return {};
    struct Hit { double t; int side; };
    std::vector<Hit> hits;
    auto add=[&](double t,int side) { if (t>=-1e-9 && t<=1.0+1e-9) hits.push_back({std::max(0.0,t),side}); };
    if (dx>0) add((a.x+a.width-x)/dx,0);
    if (dx<0) add((a.x-x)/dx,1);
    if (dy>0) add((a.y+a.height-y)/dy,2);
    if (dy<0) add((a.y-y)/dy,3);
    if (hits.empty()) return {};
    std::sort(hits.begin(),hits.end(),[](const Hit& p,const Hit& q){return p.t<q.t || (p.t==q.t && p.side<q.side);});
    const double first=hits.front().t;
    for (const auto& hit:hits) {
        if (hit.t>first+1e-9) break;
        const double px=x+dx*hit.t,py=y+dy*hit.t;
        for (std::size_t i=0;i<layout.size();++i) {
            if (i==current) continue;
            const auto& b=layout[i].bounds;
            bool match=false;
            if (hit.side<=1) {
                const double lo=std::max(a.y,b.y),hi=std::min(a.y+a.height,b.y+b.height);
                match=hi-lo>EdgeEpsilon && py>=lo && py<hi &&
                    (hit.side==0 ? Near(a.x+a.width,b.x) : Near(a.x,b.x+b.width));
            } else {
                const double lo=std::max(a.x,b.x),hi=std::min(a.x+a.width,b.x+b.width);
                match=hi-lo>EdgeEpsilon && px>=lo && px<hi &&
                    (hit.side==2 ? Near(a.y+a.height,b.y) : Near(a.y,b.y+b.height));
            }
            if (match) { auto point=ClampTo(b,px,py); return Crossing{i,point.first,point.second}; }
        }
    }
    return {};
}
}
