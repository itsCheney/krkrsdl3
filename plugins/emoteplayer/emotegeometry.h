#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace emoteplayer
{
class emotenoderef;
struct EmoteVertex
{
    float x, y; // DrawMesh NDC: (-1, -1) is the target's top-left.
    float u, v;
};

struct emoteRender // 渲染方式
{

    int type = 0; // 0:不绘制 1:icon参与网格变形和矩阵变换 2:icon只参与矩阵变换 3:layout/motion变形(无法使用网格和透明度)
    float controlPts[32] = {0.0};
    float opa = 1.0;

    bool hasStencil = false;
    std::vector<emotenoderef*> layerNode;

    // 基础参数
    float originX = 0;
    float originY = 0;
    float width = 0;
    float height = 0;
    // 计算参数  lastMat = attachMat * calcMat
    glm::mat4 attachMat = glm::mat4(1.0f);
    float currCoordx = 0, currCoordy = 0, currCoordz = 0;
    float currAngle = 0, currZx = 1.0, currZy = 1.0;
    float currSx = 0, currSy = 0;
    float currOx = 0, currOy = 0;
    uint32_t currInheritMask = 0x20007FC;
    int32_t transformOrder[4] = {0, 1, 2, 3};
    // 辅助信息
    std::string label;
};

// Cubic Bezier basis functions
inline float B0(float t) { return (1.0f - t) * (1.0f - t) * (1.0f - t); }
inline float B1(float t) { return 3.0f * t * (1.0f - t) * (1.0f - t); }
inline float B2(float t) { return 3.0f * t * t * (1.0f - t); }
inline float B3(float t) { return t * t * t; }

// Evaluate a single bicubic Bezier patch at (u, v)
// controlPts[32] = 16 control points × 2 floats (x, y) stored row-major
inline void evalBezierSurface(const float controlPts[32], float u, float v,
    float& outX, float& outY)
{
    float bu[4] = { B0(u), B1(u), B2(u), B3(u) };
    float bv[4] = { B0(v), B1(v), B2(v), B3(v) };
    float rx = 0.0f, ry = 0.0f;
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int idx = (row * 4 + col) * 2;
            float basis = bu[row] * bv[col];
            rx += controlPts[idx] * basis;
            ry += controlPts[idx + 1] * basis;
        }
    }
    outX = rx;
    outY = ry;
}

// inheritMask filters the linear transform, never the node's placement.
inline glm::mat4 buildMotionLinear(const emoteRender& method, uint32_t inheritMask)
{
    const float angle = (inheritMask & 0x10) ? method.currAngle : 0.0f;
    const float zoomX = (inheritMask & 0x20) ? method.currZx : 1.0f;
    const float zoomY = (inheritMask & 0x40) ? method.currZy : 1.0f;
    const float slantX = (inheritMask & 0x80) ? method.currSx : 0.0f;
    const float slantY = (inheritMask & 0x100) ? method.currSy : 0.0f;

    glm::mat4 linear(1.0f);
    for (const auto operation : method.transformOrder)
    {
        if (operation == 1 && angle != 0.0f)
            linear =
                glm::rotate(glm::mat4(1.0f), glm::radians(angle), glm::vec3(0.0f, 0.0f, 1.0f)) *
                linear;
        if (operation == 2 && (zoomX != 1.0f || zoomY != 1.0f))
            linear = glm::scale(glm::mat4(1.0f), glm::vec3(zoomX, zoomY, 1.0f)) * linear;
        if (operation == 3 && (slantX != 0.0f || slantY != 0.0f))
        {
            glm::mat4 shear(1.0f);
            shear[1][0] = slantX;
            shear[0][1] = slantY;
            linear = shear * linear;
        }
    }
    return linear;
}

// A point consists of its local geometry and the placements of its ancestors.
// Each placement starts inheriting at its parent, one level later than geometry.
// Keeping these contributions separate lets the reverse surface walk preserve
// spacing even when the leaf opts out of its parents' rotation/scale/shear.
struct emoteSurfaceContribution
{
    glm::vec4 offset;
    uint32_t inheritMask;
};

inline constexpr uint32_t emoteLinearInheritMask = 0x1F0;

// Walk every chain from the innermost surface to the player root. Bezier patches
// consume parent-local pixel coordinates, normalized to UV with the axis swap.
inline void evaluateSurfaceChain(const std::vector<emoteRender>& renderMethod,
                                 float u,
                                 float v,
                                 float& outClipX,
                                 float& outClipY)
{
    std::vector<emoteSurfaceContribution> points;
    points.reserve(renderMethod.size() + 1);
    uint32_t geometryMask = emoteLinearInheritMask;
    glm::vec4 trans(0.0f);
    const int surfaceCount = (int)renderMethod.size();

    for (int i = surfaceCount - 1; i >= 0; --i)
    {
        const emoteRender& surface = renderMethod[i];
        const bool innermost = i == surfaceCount - 1;
        if (innermost || surface.type == 1)
        {
            float bx = u, by = v;
            if (surface.type != 3)
            {
                const float lastU = innermost ? u : (trans.y + surface.originY) / surface.height;
                const float lastV = innermost ? v : (trans.x + surface.originX) / surface.width;
                if (surface.type == 1)
                    evalBezierSurface(surface.controlPts, lastU, lastV, bx, by);
                else
                {
                    bx = lastV;
                    by = lastU;
                }
                bx = bx * surface.width - surface.originX - surface.currOx;
                by = by * surface.height - surface.originY - surface.currOy;
            }
            // A deformation adds a displacement to the existing local point;
            // it must not turn the ancestor placements into leaf geometry.
            points.push_back({glm::vec4(bx, by, 0.0f, 0.0f) - trans, geometryMask});
        }
        // Affine ancestors transform their child coordinates directly. Their
        // sprite origin/offset belongs to their own geometry, not their children.
        trans = glm::vec4(0.0f);
        uint32_t previousMask = 0;
        glm::mat4 model(1.0f);
        for (auto& point : points)
        {
            const uint32_t mask = i == 0 || innermost ? emoteLinearInheritMask : point.inheritMask;
            if (&point == &points.front() || mask != previousMask)
            {
                model = surface.attachMat * buildMotionLinear(surface, mask);
                previousMask = mask;
            }
            point.offset = model * point.offset;
            point.inheritMask &= surface.currInheritMask;
            trans += point.offset;
        }

        glm::vec4 position =
            surface.attachMat * glm::vec4(surface.currCoordx, surface.currCoordy, 0.0f, 1.0f);
        position.w = 0.0f;
        // Coalesce fully inherited placements; the usual affine chain keeps
        // only the geometry and one placement contribution.
        if (!points.empty() && points.back().inheritMask == emoteLinearInheritMask)
            points.back().offset += position;
        else
            points.push_back({position, emoteLinearInheritMask});
        trans += position;
        geometryMask &= surface.currInheritMask;
    }

    outClipX = trans.x;
    outClipY = -trans.y;
}

// Build subdivided mesh for a given icon node
inline void buildSubdivMesh(
    const std::vector<emoteRender>& renderMethod,
    int divX, int divY,
    std::vector<EmoteVertex>& outVerts,
    std::vector<uint16_t>& outIndices)
{
    outVerts.clear();
    outIndices.clear();

    // Generate vertices
    outVerts.reserve((divX + 1) * (divY + 1));
    for (int gy = 0; gy <= divY; gy++) {
        float v = (float)gy / (float)divY;
        for (int gx = 0; gx <= divX; gx++) {
            float u = (float)gx / (float)divX;
            float clipX, clipY;
            evaluateSurfaceChain(renderMethod, u, v, clipX, clipY);
            // tessCoord in old shader was (gl_TessCoord.y, gl_TessCoord.x) = (v, u)
            outVerts.push_back({ clipX, clipY, v, u });
        }
    }

    // Generate triangle indices (2 triangles per quad)
    outIndices.reserve(divX * divY * 6);
    for (int gy = 0; gy < divY; gy++) {
        for (int gx = 0; gx < divX; gx++) {
            uint16_t i0 = (uint16_t)(gy * (divX + 1) + gx);
            uint16_t i1 = (uint16_t)(gy * (divX + 1) + gx + 1);
            uint16_t i2 = (uint16_t)((gy + 1) * (divX + 1) + gx);
            uint16_t i3 = (uint16_t)((gy + 1) * (divX + 1) + gx + 1);
            // Triangle 1: p0-p1-p2
            outIndices.push_back(i0);
            outIndices.push_back(i1);
            outIndices.push_back(i2);
            // Triangle 2: p1-p3-p2
            outIndices.push_back(i1);
            outIndices.push_back(i3);
            outIndices.push_back(i2);
        }
    }
}
// Build simple rectangle mesh (two triangles)
inline void buildRectMesh(const std::vector<emoteRender>& renderMethod,
                          std::vector<EmoteVertex>& outVerts,
                          std::vector<uint16_t>& outIndices)
{
    outVerts.clear();
    outIndices.clear();
    float cornerUV[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};

    outVerts.reserve(4);
    for (int i = 0; i < 4; i++)
    {
        float clipX, clipY;
        evaluateSurfaceChain(renderMethod, cornerUV[i][0], cornerUV[i][1], clipX, clipY);
        outVerts.push_back({clipX, clipY, cornerUV[i][1], cornerUV[i][0]});
    }

    outIndices.reserve(6);
    outIndices.push_back(0);
    outIndices.push_back(1);
    outIndices.push_back(2);
    outIndices.push_back(1);
    outIndices.push_back(3);
    outIndices.push_back(2);
}

// Shapes use the same UV -> Bezier -> node/parent -> projection path as icons.
inline void buildShapeMesh(const std::vector<emoteRender>& methods, int shapeType,
                           std::vector<EmoteVertex>& vertices,
                           std::vector<uint16_t>& indices)
{
    if (shapeType != 0 && shapeType != 1)
    {
        const bool deformed = std::any_of(methods.begin(), methods.end(),
            [](const emoteRender& m) { return m.type == 1; });
        if (deformed)
            buildSubdivMesh(methods, 8, 8, vertices, indices);
        else
            buildRectMesh(methods, vertices, indices);
        return;
    }
    // A circle (or a one-unit point marker) becomes an ellipse under affine transforms.
    // 64 segments keep the radial approximation error below 0.13%.
    constexpr int segments = 64;
    vertices.clear();
    indices.clear();
    float x, y;
    evaluateSurfaceChain(methods, 0.5f, 0.5f, x, y);
    vertices.push_back({x, y, 0.5f, 0.5f});
    for (int i = 0; i < segments; ++i)
    {
        const float angle = i * 6.28318530718f / segments;
        const float u = 0.5f + 0.5f * std::sin(angle);
        const float v = 0.5f + 0.5f * std::cos(angle);
        evaluateSurfaceChain(methods, u, v, x, y);
        vertices.push_back({x, y, v, u});
        indices.insert(indices.end(), {0, uint16_t(i + 1), uint16_t((i + 1) % segments + 1)});
    }
}

// Query the actual triangles, including rotation, reflection and mesh deformation.
// Degenerate/non-finite triangles cannot be hit; edges are included.
inline bool containsMesh(const std::vector<EmoteVertex>& vertices,
                         const std::vector<uint16_t>& indices, float x, float y)
{
    if (!std::isfinite(x) || !std::isfinite(y))
        return false;
    const auto edge = [](const EmoteVertex& a, const EmoteVertex& b, double px, double py)
    { return (double(b.x) - a.x) * (py - a.y) - (double(b.y) - a.y) * (px - a.x); };
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() ||
            indices[i + 2] >= vertices.size())
            continue;
        const auto& a = vertices[indices[i]];
        const auto& b = vertices[indices[i + 1]];
        const auto& c = vertices[indices[i + 2]];
        const double area = edge(a, b, c.x, c.y);
        if (!std::isfinite(area) || area == 0 ||
            x < std::min({a.x, b.x, c.x}) || x > std::max({a.x, b.x, c.x}) ||
            y < std::min({a.y, b.y, c.y}) || y > std::max({a.y, b.y, c.y}))
            continue;
        const double ab = edge(a, b, x, y), bc = edge(b, c, x, y), ca = edge(c, a, x, y);
        if ((ab >= 0 && bc >= 0 && ca >= 0) || (ab <= 0 && bc <= 0 && ca <= 0))
            return true;
    }
    return false;
}
} // namespace emoteplayer
