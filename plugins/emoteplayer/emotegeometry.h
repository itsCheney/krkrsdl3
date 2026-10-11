#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

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

// The transform part of a surface chain is invariant across all vertices of one
// node for a frame. Build it once, then reuse it while evaluating the UV grid.
inline void buildSurfaceMatrices(
    const std::vector<emoteRender>& renderMethod,
    std::vector<glm::mat4>& matrices)
{
    const int surfaceCount = (int)renderMethod.size();
    matrices.resize(surfaceCount);
    uint32_t currInheritMask = 0xFFFFFFF;

    for (int i = surfaceCount - 1; i >= 0; --i)
    {
        currInheritMask &= renderMethod[i].currInheritMask;
        const auto& surface = renderMethod[i];
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
        if (surface.type >= 1 && surface.type <= 2)
        {
            model = glm::translate(
                model, glm::vec3(-surface.originX - surface.currOx,
                                 -surface.originY - surface.currOy, 0.0f));
            model = glm::scale(model, glm::vec3(surface.width, surface.height, 1.0f));
        }
        matrices[i] = surface.attachMat * model;
    }
}

inline void buildGPUDeformSurfaces(
    const std::vector<emoteRender>& renderMethod,
    std::vector<glm::mat4>& matrices,
    std::vector<krkrsdl3::TVPMeshDeformSurface>& surfaces)
{
    buildSurfaceMatrices(renderMethod, matrices);
    surfaces.resize(renderMethod.size());
    for (size_t i = 0; i < renderMethod.size(); ++i)
    {
        auto& out = surfaces[i];
        const auto& in = renderMethod[i];
        std::copy(glm::value_ptr(matrices[i]), glm::value_ptr(matrices[i]) + 16, out.matrix);
        out.originX = in.originX;
        out.originY = in.originY;
        out.width = in.width;
        out.height = in.height;
        out.type = in.type;
        std::copy(std::begin(in.controlPts), std::end(in.controlPts), out.controlPts);
    }
}

inline void evaluateSurfaceChainPrepared(
    const std::vector<emoteRender>& renderMethod,
    const std::vector<glm::mat4>& matrices,
    float u, float v,
    float& outClipX, float& outClipY)
{
    float lastX = 0;
    float lastY = 0;
    glm::vec4 trans = glm::vec4(1.0f);
    const int surfaceCount = (int)renderMethod.size();

    for (int i = surfaceCount - 1; i >= 0; --i)
    {
        const auto& surface = renderMethod[i];
        const glm::mat4& model = matrices[i];

        if (surface.type == 3)
        {
            trans = model * trans;
        }
        else
        {
            if (i < surfaceCount - 1)
            {
                lastX = (trans.y + surface.originY) / surface.height;
                lastY = (trans.x + surface.originX) / surface.width;
            }
            else
            {
                lastX = u;
                lastY = v;
            }

            float bx, by;
            if (surface.type == 1)
                evalBezierSurface(surface.controlPts, lastX, lastY, bx, by);
            else
                bx = lastY, by = lastX;

            trans = model * glm::vec4(bx, by, 0.0f, 1.0f);
        }
    }

    outClipX = trans.x;
    outClipY = -trans.y;
}

// Compatibility helper for one-off evaluations. Mesh builders use the prepared
// form so matrix construction is not repeated for every vertex.
inline void evaluateSurfaceChain(
    const std::vector<emoteRender>& renderMethod,
    float u, float v,
    float& outClipX, float& outClipY)
{
    std::vector<glm::mat4> matrices;
    buildSurfaceMatrices(renderMethod, matrices);
    evaluateSurfaceChainPrepared(renderMethod, matrices, u, v, outClipX, outClipY);
}

// Build subdivided mesh for a given icon node
inline void buildSubdivMesh(
    const std::vector<emoteRender>& renderMethod,
    int divX, int divY,
    std::vector<EmoteVertex>& outVerts,
    std::vector<uint16_t>& outIndices,
    std::vector<glm::mat4>* matrixScratch = nullptr,
    bool rebuildIndices = true)
{
    std::vector<glm::mat4> localMatrices;
    auto& matrices = matrixScratch ? *matrixScratch : localMatrices;
    buildSurfaceMatrices(renderMethod, matrices);

    const size_t vertexCount = size_t(divX + 1) * size_t(divY + 1);
    outVerts.resize(vertexCount);
    size_t vertex = 0;
    for (int gy = 0; gy <= divY; gy++) {
        float v = (float)gy / (float)divY;
        for (int gx = 0; gx <= divX; gx++) {
            float u = (float)gx / (float)divX;
            float clipX, clipY;
            evaluateSurfaceChainPrepared(renderMethod, matrices, u, v, clipX, clipY);
            outVerts[vertex++] = { clipX, clipY, v, u };
        }
    }

    if (!rebuildIndices)
        return;

    const size_t indexCount = size_t(divX) * size_t(divY) * 6;
    outIndices.resize(indexCount);
    size_t index = 0;
    for (int gy = 0; gy < divY; gy++) {
        for (int gx = 0; gx < divX; gx++) {
            uint16_t i0 = (uint16_t)(gy * (divX + 1) + gx);
            uint16_t i1 = (uint16_t)(gy * (divX + 1) + gx + 1);
            uint16_t i2 = (uint16_t)((gy + 1) * (divX + 1) + gx);
            uint16_t i3 = (uint16_t)((gy + 1) * (divX + 1) + gx + 1);
            outIndices[index++] = i0;
            outIndices[index++] = i1;
            outIndices[index++] = i2;
            outIndices[index++] = i1;
            outIndices[index++] = i3;
            outIndices[index++] = i2;
        }
    }
}
// Build simple rectangle mesh (two triangles)
inline void buildRectMesh(const std::vector<emoteRender>& renderMethod,
                          std::vector<EmoteVertex>& outVerts,
                          std::vector<uint16_t>& outIndices,
                          std::vector<glm::mat4>* matrixScratch = nullptr,
                          bool rebuildIndices = true)
{
    std::vector<glm::mat4> localMatrices;
    auto& matrices = matrixScratch ? *matrixScratch : localMatrices;
    buildSurfaceMatrices(renderMethod, matrices);

    static constexpr float cornerUV[4][2] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}
    };
    outVerts.resize(4);
    for (int i = 0; i < 4; i++)
    {
        float clipX, clipY;
        evaluateSurfaceChainPrepared(renderMethod, matrices, cornerUV[i][0], cornerUV[i][1],
                                     clipX, clipY);
        outVerts[i] = {clipX, clipY, cornerUV[i][1], cornerUV[i][0]};
    }

    if (rebuildIndices)
        outIndices = {0, 1, 2, 1, 3, 2};
}

// Shapes use the same UV -> Bezier -> node/parent -> projection path as icons.
inline void buildShapeMesh(const std::vector<emoteRender>& methods, int shapeType,
                           std::vector<EmoteVertex>& vertices,
                           std::vector<uint16_t>& indices,
                           std::vector<glm::mat4>* matrixScratch = nullptr)
{
    if (shapeType != 0 && shapeType != 1)
    {
        const bool deformed = std::any_of(methods.begin(), methods.end(),
            [](const emoteRender& m) { return m.type == 1; });
        if (deformed)
            buildSubdivMesh(methods, 8, 8, vertices, indices, matrixScratch);
        else
            buildRectMesh(methods, vertices, indices, matrixScratch);
        return;
    }
    // A circle (or a one-unit point marker) becomes an ellipse under affine transforms.
    // 64 segments keep the radial approximation error below 0.13%.
    constexpr int segments = 64;
    std::vector<glm::mat4> localMatrices;
    auto& matrices = matrixScratch ? *matrixScratch : localMatrices;
    buildSurfaceMatrices(methods, matrices);

    vertices.clear();
    indices.clear();
    float x, y;
    evaluateSurfaceChainPrepared(methods, matrices, 0.5f, 0.5f, x, y);
    vertices.push_back({x, y, 0.5f, 0.5f});
    for (int i = 0; i < segments; ++i)
    {
        const float angle = i * 6.28318530718f / segments;
        const float u = 0.5f + 0.5f * std::sin(angle);
        const float v = 0.5f + 0.5f * std::cos(angle);
        evaluateSurfaceChainPrepared(methods, matrices, u, v, x, y);
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
