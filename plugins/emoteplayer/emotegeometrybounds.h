#pragma once
#include "emotecapturecache.h"
#include <array>
#include <cmath>
#include <limits>

namespace emoteplayer::performance {

// Only existing, submitted CPU vertices are eligible in v1. Shape meshes are
// intentionally excluded: they need not cover the visible player's pixels.
template <class Nodes>
Bounds submittedCPUBounds(const Nodes& nodes, int width, int height) {
    if (width <= 0 || height <= 0) return {false,{}};
    double left = width, top = height, right = 0, bottom = 0;
    bool any = false;
    for (const auto* node : nodes) {
        if (!node || !node->wasDrawn) continue;
        if (node->_useGPUDeform) return {false,{}};
        for (const auto index : node->_meshIndices)
            if (index >= node->_meshVertices.size()) return {false,{}};
        for (const auto& vertex : node->_meshVertices) {
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y)) return {false,{}};
            const double x = (double(vertex.x) + 1) * .5 * width;
            const double y = (double(vertex.y) + 1) * .5 * height;
            if (!any) { left = right = x; top = bottom = y; any = true; }
            else { left = std::min(left,x); right = std::max(right,x);
                   top = std::min(top,y); bottom = std::max(bottom,y); }
        }
    }
    if (!any) return {};
    // Clip in double before converting to int; arbitrarily large finite NDC
    // coordinates must not overflow the integer rectangle.
    return {true,{int(std::clamp(std::floor(left)-1,0.0,double(width))),
                  int(std::clamp(std::floor(top)-1,0.0,double(height))),
                  int(std::clamp(std::ceil(right)+1,0.0,double(width))),
                  int(std::clamp(std::ceil(bottom)+1,0.0,double(height)))}};
}

// Independent tightness experiment. These double intervals bound the real
// polynomial (with outward-rounded double operations), not every Metal fast-
// math rounding. Production capture therefore never consumes this result.
namespace bounds_experiment {
struct Interval { double lo = 0, hi = 0; };
inline double down(double x) { return std::nextafter(x,-std::numeric_limits<double>::infinity()); }
inline double up(double x) { return std::nextafter(x,std::numeric_limits<double>::infinity()); }
inline Interval add(Interval a,Interval b) { return {down(a.lo+b.lo),up(a.hi+b.hi)}; }
inline Interval sub(Interval a,Interval b) { return {down(a.lo-b.hi),up(a.hi-b.lo)}; }
inline Interval mul(Interval a,Interval b) {
    const std::array<double,4> v{{a.lo*b.lo,a.lo*b.hi,a.hi*b.lo,a.hi*b.hi}};
    return {down(*std::min_element(v.begin(),v.end())),up(*std::max_element(v.begin(),v.end()))};
}
inline Interval scale(Interval a,double b) { return mul(a,{b,b}); }
inline Interval divide(Interval a,double b) {
    const double x = a.lo/b, y = a.hi/b;
    return {down(std::min(x,y)),up(std::max(x,y))};
}
inline bool finite(Interval a) { return std::isfinite(a.lo) && std::isfinite(a.hi) && a.lo <= a.hi; }
inline Interval lerp(Interval a,Interval b,double t) { return add(a,scale(sub(b,a),t)); }
inline Interval blossom(std::array<Interval,4> p,double a,double b,int index) {
    // The k-th restricted Bernstein control equals the cubic blossom at
    // (a repeated 3-k times, b repeated k times), including extrapolation.
    for (int level = 0; level < 3; ++level) {
        const double t = level < 3-index ? a : b;
        for (int i = 0; i < 3-level; ++i) p[i] = lerp(p[i],p[i+1],t);
    }
    return p[0];
}
struct Surface {
    int type = 0;
    std::array<double,16> matrix{{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}};
    std::array<double,32> control{};
    double originX = 0, originY = 0, width = 1, height = 1;
};
inline Interval patch(const Surface& s,int coordinate,Interval u,Interval v) {
    std::array<std::array<Interval,4>,4> restricted{};
    for (int row = 0; row < 4; ++row) {
        std::array<Interval,4> p{};
        for (int col = 0; col < 4; ++col)
            p[col] = {s.control[(row*4+col)*2+coordinate],s.control[(row*4+col)*2+coordinate]};
        // Production evalBezierSurface applies B(row,u) * B(col,v).
        for (int col = 0; col < 4; ++col) restricted[row][col] = blossom(p,v.lo,v.hi,col);
    }
    Interval result{std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()};
    for (int col = 0; col < 4; ++col) {
        std::array<Interval,4> p{};
        for (int row = 0; row < 4; ++row) p[row] = restricted[row][col];
        for (int row = 0; row < 4; ++row) {
            auto c = blossom(p,u.lo,u.hi,row);
            if (!finite(c)) return {std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN()};
            result.lo = std::min(result.lo,c.lo); result.hi = std::max(result.hi,c.hi);
        }
    }
    return result;
}
struct Result { bool known = false; Interval x,y; };
inline Result surfaceChain(const std::vector<Surface>& surfaces) {
    if (surfaces.empty() || surfaces.size() > 32) return {};
    std::array<Interval,4> trans{{{1,1},{1,1},{1,1},{1,1}}};
    for (int n = int(surfaces.size())-1; n >= 0; --n) {
        const auto& s = surfaces[n];
        if (s.type < 0 || s.type > 3) return {};
        if (!std::isfinite(s.originX) || !std::isfinite(s.originY)) return {};
        for (double value : s.matrix) if (!std::isfinite(value)) return {};
        if (s.type == 1) for (double value : s.control) if (!std::isfinite(value)) return {};
        if (s.type != 3) {
            if (!std::isfinite(s.width) || !std::isfinite(s.height) || !s.width || !s.height) return {};
            Interval u{0,1},v{0,1};
            if (n < int(surfaces.size())-1) {
                u = divide(add(trans[1],{s.originY,s.originY}),s.height);
                v = divide(add(trans[0],{s.originX,s.originX}),s.width);
            }
            Interval x = s.type == 1 ? patch(s,0,u,v) : v;
            Interval y = s.type == 1 ? patch(s,1,u,v) : u;
            trans = {{x,y,{0,0},{1,1}}};
        }
        std::array<Interval,4> output{};
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col)
                output[row] = add(output[row],scale(trans[col],s.matrix[col*4+row]));
            if (!finite(output[row])) return {};
        }
        trans = output;
    }
    return {true,trans[0],{-trans[1].hi,-trans[1].lo}};
}
} // namespace bounds_experiment

// Diagnostic candidate only. The production CPU-only bounds function above
// deliberately remains unknown for GPU-deformed nodes until native Metal
// vertex validation establishes a safe floating-point margin.
template <class Nodes>
Bounds submittedExperimentalBounds(const Nodes& nodes,int width,int height) {
    if (width <= 0 || height <= 0) return {false,{}};
    Bounds result;
    for (const auto* node : nodes) {
        if (!node || !node->wasDrawn) continue;
        Bounds bounds;
        if (!node->_useGPUDeform) {
            std::vector<decltype(node)> one{node};
            bounds = submittedCPUBounds(one,width,height);
        } else {
            std::vector<bounds_experiment::Surface> surfaces;
            surfaces.reserve(node->_gpuDeformSurfaces.size());
            for (const auto& in : node->_gpuDeformSurfaces) {
                bounds_experiment::Surface out;
                out.type = in.type;
                std::copy(std::begin(in.matrix),std::end(in.matrix),out.matrix.begin());
                std::copy(std::begin(in.controlPts),std::end(in.controlPts),out.control.begin());
                out.originX = in.originX; out.originY = in.originY;
                out.width = in.width; out.height = in.height;
                surfaces.push_back(out);
            }
            const auto candidate = bounds_experiment::surfaceChain(surfaces);
            if (!candidate.known) return {false,{}};
            const double left = (candidate.x.lo+1)*.5*width;
            const double right = (candidate.x.hi+1)*.5*width;
            const double top = (candidate.y.lo+1)*.5*height;
            const double bottom = (candidate.y.hi+1)*.5*height;
            if (!std::isfinite(left) || !std::isfinite(right) ||
                !std::isfinite(top) || !std::isfinite(bottom)) return {false,{}};
            bounds = {true,{int(std::clamp(std::floor(left)-1,0.0,double(width))),
                            int(std::clamp(std::floor(top)-1,0.0,double(height))),
                            int(std::clamp(std::ceil(right)+1,0.0,double(width))),
                            int(std::clamp(std::ceil(bottom)+1,0.0,double(height)))}};
        }
        if (!bounds.known) return {false,{}};
        result.rect = unite(result.rect,bounds.rect);
    }
    return result;
}
} // namespace emoteplayer::performance
