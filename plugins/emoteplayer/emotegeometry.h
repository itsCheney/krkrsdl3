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

// Evaluate the full surface chain (same logic as the old tess eval shader)
// Start with UV (u,v), iterate surfaces from innermost to outermost,
// applying bezier deformation and matrix transform at each level.
// The outermost surface (index 0) includes projection and outputs clip space [-1,1].
// Inner surfaces have model-only matrices and output pixel space (relative to parent);
// their output is normalized back to UV [0,1] (with axis swap) for the next surface.
inline void evaluateSurfaceChain(
    const std::vector<emoteRender>& renderMethod,
    float u, float v,
    float& outClipX, float& outClipY)
{
    float lastX = 0;
    float lastY = 0;
    glm::vec4 trans = glm::vec4(1.0f);
    int surfaceCount = (int)renderMethod.size();
    uint32_t currInheritMask = 0xFFFFFFF;

    // Iterate in reverse: renderMethod[surfaceCount-1] is innermost (first in shader)
    for (int i = surfaceCount - 1; i >= 0; i--)
    {
        // inheritMask
        currInheritMask &= renderMethod[i].currInheritMask;
        const auto& surface = renderMethod[i];
        // Only ancestors inside the player obey the accumulated inheritance mask.
        const uint32_t mask = (i > 0 && i < surfaceCount - 1) ? currInheritMask : 0xFFFFFFFF;
        glm::mat4 model = glm::translate(glm::mat4(1.0f),
            glm::vec3(surface.currCoordx, surface.currCoordy, 0));
        if (mask & 0x10)
            model = glm::rotate(model, glm::radians(surface.currAngle), glm::vec3(0, 0, 1));
        model = glm::scale(model, glm::vec3(mask & 0x20 ? surface.currZx : 1,
                                           mask & 0x40 ? surface.currZy : 1, 1));
        glm::mat4 shear(1.0f);
        if (mask & 0x80) shear[1][0] = surface.currSx;
        if (mask & 0x100) shear[0][1] = surface.currSy;
        model = shear * model;
        // 非layout补充计算矩阵
        if (renderMethod[i].type >= 1 && renderMethod[i].type <= 2)
        {
            model = glm::translate(
                model, glm::vec3(-renderMethod[i].originX - renderMethod[i].currOx,
                                 -renderMethod[i].originY - renderMethod[i].currOy, 0.0f));
            model =
                glm::scale(model, glm::vec3(renderMethod[i].width, renderMethod[i].height, 1.0f));
        }
        // 加入attach矩阵
        model = renderMethod[i].attachMat * model;

        if (renderMethod[i].type == 3)
        {
            // Layout层 直接计算
            trans = model * trans;
        }
        else
        {
            // 获取 lastX,lastY
            if (i < surfaceCount - 1)
            {
                // Inner surfaces have model-only matrices; output is in parent's pixel space.
                // Normalize back to UV [0,1] (with axis swap: parent-Y→U, parent-X→V)
                // for the next (outer) surface's Bezier input.
                lastX = (trans.y + renderMethod[i].originY) / renderMethod[i].height; // Y → U
                lastY = (trans.x + renderMethod[i].originX) / renderMethod[i].width;  // X → V
            }
            else
            {
                lastX = u;
                lastY = v;
            }

            // Evaluate bezier surface
            float bx, by;
            if (renderMethod[i].type == 1)
            {
                evalBezierSurface(renderMethod[i].controlPts, lastX, lastY, bx, by);
            }
            else
            {
                bx = lastY, by = lastX;
            }

            // Apply transformation matrix
            trans = model * glm::vec4(bx, by, 0.0f, 1.0f);
        }
    }

    // Final Y flip (same as shader: gl_Position = lastPt * vec4(1, -1, 1, 1))
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
