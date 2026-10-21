#include "emoterunner.h"
#include "emoteframeselect.h"

#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <SDL3/SDL.h>

#include "Platform.h"
#include "TVPCompositor.h"

namespace emoteplayer
{
namespace {
double resolveFrameCoordinate(double coordinate, double origin, double extent)
{
    // Decoded frames are shared between players. Viewport-relative sentinels
    // must be resolved per instance instead of replacing shared NaN/Inf values.
    if (std::isnan(coordinate)) return -origin;
    if (std::isinf(coordinate)) return extent - origin;
    return coordinate;
}
} // namespace
void emotenoderef::checkDrawStatus(float tick, std::vector<emoteRender>& renderList, emotelimit lim)
{
    // 不绘制进行节点传递
    if (renderList.size() > 0 && renderList.back().type == 0)
    {
        isNeedDraw = false;
        return;
    }

    // 确定时间轴
    if (currentNode->frameList.size() == 0)
    {
        isNeedDraw = false;
        return;
    }
    const auto selected = selectEmoteNodeFrames(currentNode->frameList, tick, currentNode->type == 1);
    frame = selected.first;
    nextframe = selected.second;
    if (frame == nullptr || !frame->hasContent)
    {
        isNeedDraw = false;
        return;
    }

    // 节点基础信息获取
    isNeedDraw = true;
    isIcon = false;
    isLayout = false;
    isShape = false;
    // Resolving frame->src parses it with istringstream and walks the source
    // and motion maps. The answer only changes with the frame, its src text,
    // the engine's main file or a newly attached file (_attach only grows),
    // so resolve once and reuse it while the node keeps showing this frame.
    using Source = SourceResolution::Kind;
    const emotefile* mainFile = refTop->_mainfile;
    const size_t attachCount = refTop->_attach.size();
    if (_source.kind == Source::Unresolved || _source.frame != frame || _source.mainFile != mainFile ||
        _source.attachCount != attachCount || _source.src != frame->src)
    {
        SourceResolution resolved;
        resolved.frame = frame;
        resolved.src = frame->src;
        resolved.mainFile = mainFile;
        resolved.attachCount = attachCount;
        resolved.icon = currentNode->_filePtr->findsourceByName(frame->src);
        if (resolved.icon != nullptr)
            resolved.kind = Source::Icon;
        else if ((resolved.motion = refTop->findmotionByName(frame->src)) != nullptr)
            resolved.kind = Source::Motion;
        else if (strcmp(frame->src.c_str(), "layout") == 0 || strcmp(frame->src.c_str(), "clip") == 0)
            resolved.kind = Source::Layout;
        else
        {
            std::istringstream iss(frame->src);
            std::string token;
            std::getline(iss, token, '/');
            if (token == "src")
            {
                // emote 拼接前缀 "src/<type>/<name>"，跳过第一段。
                // 此前直接拿 "src" 判类型，落入 unsupported 分支被丢弃，
                // shape 判定层因此收集不到区域
                std::getline(iss, token, '/');
            }
            if (strcmp(token.c_str(), "blank") == 0)
            {
                std::getline(iss, token, ':');
                resolved.blankWidth = std::stoi(token);
                std::getline(iss, token, ':');
                resolved.blankHeight = std::stoi(token);
                std::getline(iss, token, ':');
                resolved.blankOriginX = std::stoi(token);
                std::getline(iss, token, ':');
                resolved.blankOriginY = std::stoi(token);
                resolved.kind = Source::Blank;
            }
            else if (strcmp(token.c_str(), "shape") == 0)
                resolved.kind = Source::Shape;
            else
            {
                // Logged when the frame is first resolved, not on every tick.
                TVPConsoleLog("source unsupported!!!--->%s", frame->src.c_str());
                resolved.kind = Source::Unsupported;
            }
        }
        _source = std::move(resolved);
    }
    currentMtn = _source.kind == Source::Motion ? _source.motion : nullptr;
    if (_source.kind == Source::Icon)
    {
        isIcon = true;

        if (_source.icon != ic) // 直接比对ic
        {
            ic = _source.icon;
            if (!refTop->samplingPhysics) ic->ensureLoad();
            width = ic->width;
            height = ic->height;
            originX = ic->originX;
            originY = ic->originY;
        }
        // 设置混色
        currbm = frame->bm;
    }
    else if (_source.kind == Source::Motion || _source.kind == Source::Layout)
    {
        // 部分导出的 PSB 会剥离 shape 帧的 src 字段
        //（content 仅剩 coord/mask/zx/zy），解析后 src 落到默认值 "layout"，
        // 此前一律当 layout 处理，shape 节点（node type==1）的判定区域
        // 收集不到。type==1 即 shape 层，按 shape 处理并记录判定区域
        if (strcmp(frame->src.c_str(), "layout") == 0 && currentNode->type == 1)
        {
            isShape = true;
            isNeedDraw = false;
            // shape 像素尺寸 = zx/zy * 16 (PSB 规范: 单元正方形为 16x16)
            width = height = 16.0f;
            originX = width / 2;
            originY = height / 2;
        }
        else
        {
            isLayout = true;

            // 直接用父类提供的区域
            if (width != lim.width || height != lim.height)
            {
                width = lim.width;
                height = lim.height;
                originX = lim.originX;
                originY = lim.originY;
            }
        }
    }
    else if (_source.kind == Source::Blank)
    {
        if (width != _source.blankWidth || height != _source.blankHeight)
        {
            width = _source.blankWidth;
            height = _source.blankHeight;
        }
        originX = _source.blankOriginX;
        originY = _source.blankOriginY;
    }
    else if (_source.kind == Source::Shape)
    {
        // shape节点: 不绘制但记录区域信息
        // shape的像素尺寸 = zx/zy * 16 (PSB规范: 单元正方形为16x16)
        // shape子类型从src的第二部分获取: rect(默认)、circle、point、quad
        isShape = true; // 标记 shape 判定层
        isNeedDraw = false;
        width = height = 16.0f;
        originX = width / 2;
        originY = height / 2;
    }
    else
    {
        isNeedDraw = false;
        return;
    }
}
void emotenoderef::progress(float tick, std::vector<emoteRender>& renderList, emotelimit lim)
{
    if (refTop->samplingPhysics)
    {
        // Physics reads only the anchor layers' positions; the rest of the
        // model needs no evaluation for it.
        const auto key = refTop->physicsAnchorKey();
        if (_anchorKey != key)
        {
            _anchorKey = key;
            _leadsToAnchor = refTop->leadsToPhysicsAnchor(currentNode);
        }
        if (!_leadsToAnchor) return;
    }
    const Uint64 nodeProfileStarted = SDL_GetTicksNS();
    // 参数化时可能改变
    currTick = tick;
    // 对于motion，增加终结机制, 即无法越过selfSyncTime
    if (currentNode->_filePtr->isMotion && currTick > currentNode->_rootmotion->selfSyncTime &&
        currentNode->frameList.size() > 1 &&
        currentNode->frameList.at(currentNode->frameList.size() - 2)->type == 2 &&
        currentNode->frameList.at(currentNode->frameList.size() - 1)->type == 0)
        currTick = currentNode->_rootmotion->selfSyncTime;
    // 再来一个非时间戳节点, 采用时间永驻机制
    if (currentNode->_filePtr->isMotion && currentNode->frameList.size() == 2 &&
        currentNode->frameList.at(0)->type == 2 && currentNode->frameList.at(1)->type == 0)
        currTick = currentNode->frameList.at(0)->time;
    // 再来一个3->0的拦截机制，感觉确实未能理解motion的运行，只能凑合着搞了
    if (currentNode->_filePtr->isMotion && currentNode->frameList.size() > 1 &&
        currentNode->frameList.at(currentNode->frameList.size() - 1)->type == 0 &&
        currentNode->frameList.at(currentNode->frameList.size() - 2)->type == 3)
    {
        currTick = std::min(
            currTick, (float)currentNode->frameList.at(currentNode->frameList.size() - 2)->time);
    }
    // 不会处理，先跳过
    if (currentNode->type == 12)
    {
        checkDrawStatus(currTick, renderList, lim);
        isNeedDraw = true;
        isLayout = true;
        originX = lim.originX;
        originY = lim.originY;
        width = lim.width;
        height = lim.height;
    }
    // 对于参数节点 进行参数反查来定位tick
    else if (currentNode->isParameterize)
    {
        if (refMtn != nullptr)
        {
            float new_tick = refMtn->getTickByIdx(currentNode->parameterIdx);
            if (new_tick >= 0.0)
            {
                checkDrawStatus(new_tick, renderList, lim);
                currTick = new_tick;
            }
            else
                isNeedDraw = false;
        }
        else
            isNeedDraw = false;
    }
    else
    {
        // 时间戳判断 绘制信息检测
        checkDrawStatus(currTick, renderList, lim);
    }

    // 注意：这里不能提前 return！官方实现中 isNeedDraw=false 的节点
    // 仍会继续构建 renderMethod 并递归所有子节点（父容器节点某些帧无内容
    // 但子孙 icon 可见是正常结构）。提前剪枝会导致整棵子树不被 progress，
    // 子孙部件消失；蒙版层（layerNode）不被绘制时 hasStencil 会被置 false，
    // 主体部件失去蒙版裁剪，纯色填充部件（bm21）以完整形态显示为色块。
    // 子孙的可见性由它们自己的 checkDrawStatus 与 draw() 中的 isNeedDraw 判定。
    // 修复（修复部分游戏emote立绘消失以及DRACU-RIOT!菜单问题。）

    // 构建渲染方法
    // renderList is the ancestors' chain, shared by the whole walk: this node
    // pushes its own entry, may adjust the root entry, and undoes both before
    // returning. Copying the chain into every node cost more than evaluating
    // the node. Only drawable icons keep a copy (renderMethod), for draw and
    // hit tests.
    std::vector<emoteRender>& chain = renderList;
    bool pushed = false;
    glm::mat4 savedRootMat(1.0f);
    bool savedRootStencil = false;
    size_t savedRootLayers = 0;
    if (!chain.empty())
    {
        savedRootMat = chain.front().attachMat;
        savedRootStencil = chain.front().hasStencil;
        savedRootLayers = chain.front().layerNode.size();
    }
    if (!frame || (!isNeedDraw && (width == 0 || height == 0)) || currentNode->type == 7)
    {
        originX = lim.originX;
        originY = lim.originY;
        width = lim.width;
        height = lim.height;
    }
    else
    {
        {
        // 基础参数
        if (nextframe != nullptr &&
            ((frame->type != 2) ||
             (frame->type == 2 && nextframe->type == 2))) // 存在下一帧则对关键帧进行插值
        {
            const double x = resolveFrameCoordinate(frame->coordX, lim.originX, lim.width);
            const double y = resolveFrameCoordinate(frame->coordY, lim.originY, lim.height);
            const double nextX = resolveFrameCoordinate(nextframe->coordX, lim.originX, lim.width);
            const double nextY = resolveFrameCoordinate(nextframe->coordY, lim.originY, lim.height);

            // 坐标
            currCoordx =
                (x + (nextX - x) /
                                     (nextframe->time - frame->time) * (currTick - frame->time));
            currCoordy =
                (y + (nextY - y) /
                                     (nextframe->time - frame->time) * (currTick - frame->time));
            currCoordz =
                (frame->coordZ + (nextframe->coordZ - frame->coordZ) /
                                     (nextframe->time - frame->time) * (currTick - frame->time));
            // 透明度
            currOpa = (frame->opa + (nextframe->opa - frame->opa) /
                                        (nextframe->time - frame->time) * (currTick - frame->time));
            // 变换参数(太sb了，感觉180才是分界点)
            if (nextframe->angle < 180 && frame->angle > 180) // 从 小360 到大0
            {
                currAngle = (frame->angle - 360 +
                             (nextframe->angle + 360 - frame->angle) /
                                 (nextframe->time - frame->time) * (currTick - frame->time));
            }
            else if (nextframe->angle > 180 && frame->angle < 180) // 从 大0 到 小360
            {
                currAngle =
                    (frame->angle + (nextframe->angle - 360 - frame->angle) /
                                        (nextframe->time - frame->time) * (currTick - frame->time));
            }
            else
            {
                currAngle =
                    (frame->angle + (nextframe->angle - frame->angle) /
                                        (nextframe->time - frame->time) * (currTick - frame->time));
            }
            currSx = (frame->sx + (nextframe->sx - frame->sx) / (nextframe->time - frame->time) *
                                      (currTick - frame->time));
            currSy = (frame->sy + (nextframe->sy - frame->sy) / (nextframe->time - frame->time) *
                                      (currTick - frame->time));
            currZx = (frame->zx + (nextframe->zx - frame->zx) / (nextframe->time - frame->time) *
                                      (currTick - frame->time));
            currZy = (frame->zy + (nextframe->zy - frame->zy) / (nextframe->time - frame->time) *
                                      (currTick - frame->time));
            currOx = (frame->ox + (nextframe->ox - frame->ox) / (nextframe->time - frame->time) *
                                      (currTick - frame->time));
            currOy = (frame->oy + (nextframe->oy - frame->oy) / (nextframe->time - frame->time) *
                                      (currTick - frame->time));
            // 偏移
            currTimeOffset = (frame->timeOffset + (nextframe->timeOffset - frame->timeOffset) /
                                                      (nextframe->time - frame->time) *
                                                      (currTick - frame->time));
            // 网格参数
            if (frame->hasbp || nextframe->hasbp)
            {
                isNeedBp = true;
                for (size_t i = 0; i < 32; i++)
                    currbp[i] = (frame->bp[i] + (nextframe->bp[i] - frame->bp[i]) /
                                                    (nextframe->time - frame->time) *
                                                    (currTick - frame->time));
            }
            else
                isNeedBp = false;
        }
        else
        {
            // 计算坐标
            currCoordx = resolveFrameCoordinate(frame->coordX, lim.originX, lim.width);
            currCoordy = resolveFrameCoordinate(frame->coordY, lim.originY, lim.height);
            currCoordz = frame->coordZ;
            // 透明度
            currOpa = frame->opa;
            // 变换参数
            currAngle = frame->angle;
            currSx = frame->sx, currSy = frame->sy;
            currZx = frame->zx, currZy = frame->zy;
            currOx = frame->ox, currOy = frame->oy;
            // 偏移
            currTimeOffset = frame->timeOffset;
            // 网格参数
            if (frame->hasbp)
            {
                isNeedBp = true;
                for (size_t i = 0; i < 32; i++)
                    currbp[i] = frame->bp[i];
            }
            else
                isNeedBp = false;
        }

        }

        // 有深度信息时，穿透到最顶层
        if (chain.size() > 0 && currCoordz != 0.0)
        {
            chain.at(0).attachMat =
                glm::translate(chain.at(0).attachMat, glm::vec3(0.0, 0.0, currCoordz));
        }

        // 有模板信息时，穿透到最顶层
        if (chain.size() > 0 && currentNode->stencilCompositeMaskLayerList.size() > 0 && currentNode->type == 12)
        {
            chain.at(0).hasStencil = true;
            // The mask layers are found by name once; the refs live as long as
            // this one (both are rebuilt when the motion's nodes change).
            if (!_stencilResolved)
            {
                _stencilResolved = true;
                _stencilRefs.clear();
                for (const auto& nodeName : currentNode->stencilCompositeMaskLayerList)
                {
                    // 让父类去找节点
                    emotenoderef* tmpNode = refMtn->getNodeRef(currentNode->_rootmotion->getNodeByName(nodeName));
                    if (tmpNode != nullptr)
                        _stencilRefs.push_back(tmpNode);
                }
            }
            chain.at(0).layerNode.insert(chain.at(0).layerNode.end(), _stencilRefs.begin(), _stencilRefs.end());
        }

        // Shape zoom stays in the shared transform, including frame interpolation.
        if (isShape && frame->src.find("shape/point") != std::string::npos)
            width = height = 2.0f; // one local unit around the node's center
        if (isShape)
        {
            originX = width * 0.5f;
            originY = height * 0.5f;
        }

        // 渲染信息记录
        chain.emplace_back();
        pushed = true;
        emoteRender& emt = chain.back();
        if (isLayout)
            emt.type = 3;
        else
            emt.type = 2;
        if (isNeedBp)
        {
            for (size_t i = 0; i < 32; i++)
                emt.controlPts[i] = currbp[i];
            emt.type = 1;
        }
        emt.opa = currOpa;
        // 矩阵计算信息
        emt.currCoordx = currCoordx;
        emt.currCoordy = currCoordy;
        emt.currAngle = currAngle;
        emt.currSx = currSx;
        emt.currSy = currSy;
        emt.currZx = currZx;
        emt.currZy = currZy;
        emt.currOx = currOx;
        emt.currOy = currOy;
        emt.currInheritMask = currentNode->inheritMask;
        std::copy(std::begin(currentNode->transformOrder), std::end(currentNode->transformOrder),
                  std::begin(emt.transformOrder));
        // fbo信息
        emt.originX = originX;
        emt.originY = originY;
        emt.width = width;
        emt.height = height;
    }
    _renderZ = chain.empty() ? 0.0f : chain.front().attachMat[3][2];
    if (isIcon && !refTop->samplingPhysics)
        renderMethod.assign(chain.begin(), chain.end()); // reuses this node's storage
    else
        renderMethod.clear();

    if (refTop->samplingPhysics && frame && !currentNode->removed)
        refTop->recordPhysicsAnchor(currentNode->label, chain);

    if (!refTop->samplingPhysics && isShape && frame && !currentNode->removed && refMtn && !chain.empty())
    {
        int shapeType = 2;
        if (frame->src.find("shape/circle") != std::string::npos) shapeType = 1;
        else if (frame->src.find("shape/point") != std::string::npos) shapeType = 0;
        else if (frame->src.find("shape/quad") != std::string::npos) shapeType = 3;
        const Uint64 shapeBuildStarted = SDL_GetTicksNS();
        refMtn->recordShape(currentNode->label, shapeType, chain,
                            lim.viewW > 0 ? lim.viewW : chain.front().width,
                            lim.viewH > 0 ? lim.viewH : chain.front().height);
        krkrsdl3::TVPRecordEmoteShapeBuild(SDL_GetTicksNS() - shapeBuildStarted, 0);
    }

    // Deformed icon meshes can be evaluated directly in the Metal vertex
    // shader. Other backends retain the exact CPU geometry path.
    auto* backend = krkrsdl3::TVPGetRenderBackend();
    const bool gpuCapable = backend && backend->SupportsMeshDeformation();
    _useGPUDeform = false;
    _gpuDeformSurfaces.clear();
    if (!refTop->samplingPhysics && isIcon && isNeedDraw && chain.size() > 0)
    {
        // 确定细分等级
        int div = currentNode ? (int)currentNode->meshDivision : 0;
        if (div < 2) div = 8; // 默认8x8，匹配原曲面细分着色器的细分等级
        div = std::min(div, 254); // (div + 1)^2 must fit uint16_t indices
        // 检查是否包含mesh变形
        bool containsMesh = false;
        for (const auto& itm : chain)
        {
            if (itm.type == 1)
            {
                containsMesh = true;
                break;
            }
        }
        // 变换
        const Uint64 meshBuildStarted = SDL_GetTicksNS();
        _meshDivX = div;
        _meshDivY = div;
        if (containsMesh && gpuCapable &&
            buildGPUDeformSurfaces(chain, _gpuDeformSurfaces))
        {
            _useGPUDeform = true;
            _meshVertices.clear();
        }
        else if (containsMesh)
        {
            const size_t expectedIndices = size_t(div) * size_t(div) * 6;
            const bool rebuildIndices = _meshIndices.size() != expectedIndices;
            buildSubdivMesh(chain, _meshDivX, _meshDivY, _meshVertices, _meshIndices,
                            rebuildIndices);
        }
        else
        {
            const bool rebuildIndices = _meshIndices.size() != 6;
            buildRectMesh(chain, _meshVertices, _meshIndices, rebuildIndices);
        }
        const uint64_t meshVertexCount = containsMesh
            ? uint64_t(div + 1) * uint64_t(div + 1)
            : uint64_t(_meshVertices.size());
        krkrsdl3::TVPRecordEmoteMeshBuild(
            SDL_GetTicksNS() - meshBuildStarted,
            meshVertexCount,
            containsMesh && !_useGPUDeform ? meshVertexCount : 0);
    }
    else
    {
        // Hidden/non-icon nodes keep allocated storage so reappearing animations
        // do not churn heap allocations. draw() already rejects non-visible nodes.
        _meshVertices.clear();
        _useGPUDeform = false;
        _gpuDeformSurfaces.clear();
    }

    // Local node work ends here. Recursive children/submotions account for
    // themselves so node time can be summed without double-counting recursion.
    krkrsdl3::TVPRecordEmoteNodeProgress(SDL_GetTicksNS() - nodeProfileStarted);

    // 传递给子类: 通过_parentMotion查找对应ref，避免创建重复状态
    if (refMtn != nullptr)
    {
        for (auto ch : currentNode->children)
        {
            emotenoderef* childRef = refMtn->getNodeRef(ch);
            if (childRef)
            {
                // Preserve the target viewport through nested motions.
                childRef->progress(tick, chain,
                                   {originX, originY, width, height, lim.zMax, lim.viewW,
                                    lim.viewH});
            }
        }

        // 处理子motion: 创建子emotemotionref递归处理
        if (currentMtn != nullptr)
        {
            // Sub-motion refs are pooled per owning emotemotion and reused across
            // frames. A steady animation reaches its high-water mark once and then
            // allocates nothing; only the active set is re-collected each frame.
            const Uint64 submotionRebuildStarted = SDL_GetTicksNS();
            uint64_t submotionCreated = 0;
            {
                auto& pool = refMtn->_subMotionPool[currentMtn];
                size_t& used = refMtn->_subMotionUsed[currentMtn];
                if (used == pool.size())
                {
                    pool.push_back(std::make_unique<emotemotionref>(currentMtn, refTop, this));
                    submotionCreated = 1;
                }
                currentMtnRef = pool[used++].get();
                // Reused entries can carry a different parent node than last frame.
                currentMtnRef->parent = this;
                currentMtnRef->label = currentNode ? currentNode->label : "";
            }
            refMtn->_subMotionRefs.push_back(currentMtnRef);
            krkrsdl3::TVPRecordEmoteSubmotionRebuild(
                SDL_GetTicksNS() - submotionRebuildStarted, submotionCreated);
            currentMtnRef->progress(tick + currTimeOffset, chain,
                            {originX, originY, width, height, lim.zMax, lim.viewW, lim.viewH});
        }
    }

    // Hand the chain back as it came in.
    if (pushed)
        chain.pop_back();
    if (!chain.empty())
    {
        chain.front().attachMat = savedRootMat;
        chain.front().hasStencil = savedRootStencil;
        chain.front().layerNode.resize(savedRootLayers);
    }
}
bool emotenoderef::containsCurrentMesh(float x, float y) const
{
    if (!_useGPUDeform)
        return containsMesh(_meshVertices, _meshIndices, x, y);

    // Hit testing is much less frequent than rendering. Materialize the exact
    // CPU geometry only when a query is made instead of paying this cost every frame.
    std::vector<EmoteVertex> vertices;
    std::vector<uint16_t> indices;
    buildSubdivMesh(renderMethod, _meshDivX, _meshDivY, vertices, indices);
    return containsMesh(vertices, indices, x, y);
}

bool emotenoderef::nearCurrentMesh(float x, float y, float margin) const
{
    if (!_useGPUDeform)
        return nearMesh(_meshVertices, _meshIndices, x, y, margin);
    std::vector<EmoteVertex> vertices;
    std::vector<uint16_t> indices;
    buildSubdivMesh(renderMethod, _meshDivX, _meshDivY, vertices, indices);
    return nearMesh(vertices, indices, x, y, margin);
}

bool emotenoderef::canDraw(krkrsdl3::iTVPRenderBackend* renderer) const
{
    if (!isNeedDraw || !isIcon || renderMethod.size() < 1 || currentNode->removed)
        return false; // 跳过无需绘制的 和 非icon的 和 无method 的节点
    if (!renderer || !ic || !ic->selftexture)
        return false;

    // CPU and GPU deformation paths carry different geometry payloads.
    if (_useGPUDeform)
        return !_gpuDeformSurfaces.empty();
    return !_meshVertices.empty() && !_meshIndices.empty();
}

bool emotenoderef::renderMask(krkrsdl3::iTVPRenderBackend* renderer, void* maskTarget, emotelimit lim)
{
    // 提前绘制好蒙版目标（不考虑复合蒙版的情况）
    if (!needsMask() || maskTarget == nullptr)
        return false;
    // Count source-node groups separately from clears. This is an identity
    // hash for profiling, not a content-validity key for a future cache.
    uint64_t group = 1469598103934665603ull;
    for (auto* maskLayer : renderMethod.at(0).layerNode)
        group = (group ^ reinterpret_cast<uintptr_t>(maskLayer)) * 1099511628211ull;
    krkrsdl3::TVPRecordEmoteMaskGroup(group);
    renderer->SetTarget(maskTarget);
    renderer->ClearTarget(true);
    krkrsdl3::TVPRecordEmoteMaskClear();
    bool hasDraw = false;
    for (auto maskLayer : renderMethod.at(0).layerNode)
    {
        if (maskLayer != nullptr && maskLayer->currOpa > 0)
        {
            hasDraw = true;
            if (maskLayer->draw(renderer, maskTarget, lim, nullptr))
                krkrsdl3::TVPRecordEmoteMaskDraw();
        }
    }
    // 排除异常蒙版
    return hasDraw;
}

bool emotenoderef::draw(krkrsdl3::iTVPRenderBackend* renderer, void* target, emotelimit lim, void* maskTarget)
{
    if (!target || !canDraw(renderer))
        return false;
    const bool masked = renderMask(renderer, maskTarget, lim);
    return drawWithMask(renderer, target, masked ? maskTarget : nullptr);
}

bool emotenoderef::drawWithMask(krkrsdl3::iTVPRenderBackend* renderer, void* target, void* mask)
{
    // 主体绘制（SetTarget 内部完成视口/深度/混合基础状态设置）
    renderer->SetTarget(target);
    renderer->SetMask(mask);

    // 透明度与混色
    float totalOpa = currOpa;
    for (size_t i = 0; i < renderMethod.size(); i++)
        totalOpa *= renderMethod.at(i).opa;
    int blendMode = currbm;
    float uniformColor[4] = {0, 0, 0, 0};
    float colorModulation[4] = {1, 1, 1, 1};
    if (blendMode == 21 && frame)
    {
        uniformColor[0] = ((frame->color) & 0xFF) / 255.0f;
        uniformColor[1] = ((frame->color >> 8) & 0xFF) / 255.0f;
        uniformColor[2] = ((frame->color >> 16) & 0xFF) / 255.0f;
        uniformColor[3] = ((frame->color >> 24) & 0xFF) / 255.0f;
    }
    else if (frame && frame->hasColor && static_cast<uint32_t>(frame->color) != 0xff808080u)
    {
        // bm!=21 时 color 是乘性染色。0xff808080 是 PSB 中性灰，必须保持原图；
        // 未写 color 时 hasColor=false，也不能用默认 0 去乘（会把虹膜乘成黑/灰）。
        const uint32_t tint = static_cast<uint32_t>(frame->color);
        colorModulation[0] = (tint & 0xFF) / 255.0f;
        colorModulation[1] = ((tint >> 8) & 0xFF) / 255.0f;
        colorModulation[2] = ((tint >> 16) & 0xFF) / 255.0f;
        colorModulation[3] = ((tint >> 24) & 0xFF) / 255.0f;
    }
    renderer->SetBlendMode(blendMode, blendMode == 21 ? uniformColor : nullptr);
    if (blendMode == 6 || totalOpa <= 0)
        return false; // 该模式不绘制（GL 后端原行为）

    if (_useGPUDeform)
    {
        if (renderer->DrawDeformedMesh(_meshDivX, _meshDivY,
                                       _gpuDeformSurfaces.data(),
                                       static_cast<int>(_gpuDeformSurfaces.size()),
                                       ic->selftexture, totalOpa, colorModulation))
            return true;

        // Backend declined the accelerated draw after prepare; preserve behavior
        // by materializing the same CPU geometry for this draw.
        std::vector<EmoteVertex> fallbackVertices;
        std::vector<uint16_t> fallbackIndices;
        buildSubdivMesh(renderMethod, _meshDivX, _meshDivY, fallbackVertices, fallbackIndices);
        renderer->DrawMesh((const float*)fallbackVertices.data(), (int)fallbackVertices.size(),
                           fallbackIndices.data(), (int)fallbackIndices.size(), ic->selftexture,
                           totalOpa, colorModulation);
        return true;
    }

    renderer->DrawMesh((const float*)_meshVertices.data(), (int)_meshVertices.size(),
                       _meshIndices.data(), (int)_meshIndices.size(), ic->selftexture, totalOpa,
                       colorModulation);
    return true;
}
float emotenoderef::getCurrentRenderZ()
{
    return _renderZ;
}

emotemotionref::~emotemotionref()
{
    // _subMotionRefs only borrows; _subMotionPool owns and releases recursively.
    _subMotionRefs.clear();
    _subMotionUsed.clear();
    _subMotionPool.clear();
}
float emotemotionref::getTickByIdx(int32_t idx)
{
    if (idx >= currentMotion->parameter.size() || idx < 0)
        return -1.0f;
    float currVal = 0;
    // file系控制
    if (!refTop->getControlVariableByKey(currentMotion->parameter.at(idx)->controlKey(), currVal) &&
        !currentMotion->_filePtr->getTickByName(currentMotion->parameter.at(idx)->id, currVal))
    {
        if (currentMotion->_filePtr->isMotion)
        {
            // mtn系控制
            auto itmMap = currentMotion->parameterCache.find(currentMotion->parameter.at(idx)->id);
            if (itmMap != currentMotion->parameterCache.end())
            {
                if (currentMotion->lastTime < 0)
                    return itmMap->second;
                else
                {
                    emoteVar* tmpV = currentMotion->parameter.at(idx);
                    return itmMap->second * currentMotion->lastTime / (tmpV->rangeEnd + 1 - tmpV->rangeBegin); 
                }
            }
            if (parent && parent->refMtn->parent)
            {
                // label逆查找的构造系列(两层)
                std::string fullvals = parent->refMtn->parent->currentNode->label + "/" +
                                       parent->currentNode->label + "/" +
                                       currentMotion->parameter.at(idx)->id;
                fullvals.erase(std::remove(fullvals.begin(), fullvals.end(), '\0'), fullvals.end());
                fullvals.append(1, '\0');
                tjs_real retV = 0.0;
                if (refTop->getTickByName(fullvals, retV))
                {
                    return retV;
                }

                // label逆查找的构造系列(一层)
                fullvals = parent->refMtn->parent->currentNode->label + "/" +
                                       currentMotion->parameter.at(idx)->id;
                fullvals.erase(std::remove(fullvals.begin(), fullvals.end(), '\0'), fullvals.end());
                fullvals.append(1, '\0');
                retV = 0.0;
                if (refTop->getTickByName(fullvals, retV))
                {
                    return retV;
                }
            }
        }
    }
    return currentMotion->parameter.at(idx)->transToTick(currVal);
}
emotenoderef* emotemotionref::getNodeRef(emotenode* node)
{
    const auto found = _nodeIndex.find(node);
    return found == _nodeIndex.end() ? nullptr : &_nodeCache[found->second];
}
void emotemotionref::recordShape(const std::string& label, int shapeType,
                                  const std::vector<emoteRender>& methods,
                                  float viewW, float viewH)
{
    // Reuse each slot's capacity across frames; the geometry is rebuilt from
    // this frame's chain only if a script asks for it.
    if (_shapeCount == shapeNodeAreas.size()) shapeNodeAreas.emplace_back();
    emoterect& area = shapeNodeAreas[_shapeCount++];
    area.label = label;
    area.shapeType = shapeType;
    area.methods.assign(methods.begin(), methods.end());
    area.viewW = viewW;
    area.viewH = viewH;
    area.built = false;
}
const emoterect& emoterect::geometry() const
{
    if (built) return *this;
    built = true;
    buildShapeMesh(methods, shapeType, vertices, indices);
    float minX = 1, minY = 1, maxX = -1, maxY = -1;
    if (!vertices.empty())
    {
        minX = maxX = vertices.front().x;
        minY = maxY = vertices.front().y;
    }
    for (const auto& v : vertices)
    {
        minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
        minY = std::min(minY, v.y); maxY = std::max(maxY, v.y);
    }
    left = (minX + 1) * 0.5f * viewW;
    top = (minY + 1) * 0.5f * viewH;
    width = (maxX - minX) * 0.5f * viewW;
    height = (maxY - minY) * 0.5f * viewH;
    return *this;
}
void emotemotionref::progress(float tick, std::vector<emoteRender>& renderList, emotelimit lim)
{
    _shapeCount = 0;
    _masksPrepared = false; // prepared masks belong to the previous geometry

    if (currentMotion == nullptr) return;

    // Motion topology 通常在整个播放期间不变。只在 nodeList 真正变化时
    // 重建 refs；否则保留每个 node 的 mesh/vector capacity 跨帧复用。
    const size_t count = currentMotion->nodeList.size();
    bool topologyChanged = _nodeCache.size() != count;
    if (!topologyChanged)
    {
        for (size_t i = 0; i < count; ++i)
        {
            if (_nodeCache[i].currentNode != currentMotion->nodeList[i])
            {
                topologyChanged = true;
                break;
            }
        }
    }
    if (topologyChanged)
    {
        _nodeCache.clear();
        _nodeIndex.clear();
        _nodeCache.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            _nodeCache.emplace_back(currentMotion->nodeList[i], refTop, this);
            _nodeIndex.emplace(currentMotion->nodeList[i], i);
        }
        // Pooled sub-motion refs hold emotenoderef* parents that point into
        // _nodeCache. Rebuilding it invalidates them, so the pool cannot survive.
        _subMotionRefs.clear();
        _subMotionUsed.clear();
        _subMotionPool.clear();
        _drawNodes.clear(); _drawScratch.clear(); _drawStack.clear();
    }

    // The active sub-motion set can change with keyframes, so it is re-collected
    // every frame. The ref objects are pooled by owning motion and survive, which
    // keeps their node/mesh storage warm instead of churning the heap.
    const Uint64 submotionCleanupStarted = SDL_GetTicksNS();
    for (auto& ref : _nodeCache)
        ref.currentMtnRef = nullptr;
    _subMotionRefs.clear();
    for (auto& entry : _subMotionUsed)
        entry.second = 0;
    krkrsdl3::TVPRecordEmoteSubmotionRebuild(
        SDL_GetTicksNS() - submotionCleanupStarted, 0);

    //  对每个layer节点调用对应的ref->progress
    //  注意: ref的progress内部会通过_parentMotion递归处理children和sub-motion
    for (auto ch : currentMotion->layer)
    {
        float actualTick = tick;
        if (currentMotion->loopTime >= 0)
        {
            if (currentMotion->loopTime == 0)
                actualTick = std::fmod(tick, currentMotion->lastTime);
            else
                actualTick = std::fmod(tick, currentMotion->loopTime);
        }

        // Layers share the caller's chain; a type 2 layer starts without the
        // inherited mask, which is put back afterwards.
        const bool dropMask = renderList.size() > 0 && ch->type == 2;
        bool keptStencil = false;
        if (dropMask)
        {
            keptStencil = renderList.front().hasStencil;
            renderList.front().hasStencil = false;
            _keptLayers.swap(renderList.front().layerNode);
        }

        emotenoderef* ref = getNodeRef(ch);
        if (ref)
        {
            ref->progress(actualTick, renderList, lim);
        }
        if (dropMask)
        {
            renderList.front().hasStencil = keptStencil;
            _keptLayers.swap(renderList.front().layerNode);
            _keptLayers.clear();
        }
    }
    shapeNodeAreas.resize(_shapeCount);
    // The draw list is built once, by emoteengine::progress for the main ref.
    // Its walk already descends into sub-motion node caches; building and
    // sorting a list at every sub-motion level was never read.
}
void emotemotionref::collectDrawNodes()
{
    _drawScratch.clear(); _drawStack.clear();
    for (auto it = _nodeCache.begin();
        it != _nodeCache.end(); ++it)
    {
        _drawStack.push_back(&(*it));
    }
    // 再递归获取全部
    while (!_drawStack.empty())
    {
        emotenoderef* current = _drawStack.back();
        _drawStack.pop_back();

        if (current->currentMtn == nullptr)
        {
            _drawScratch.push_back(current);
        }
        else if (current->currentMtnRef)
        {
            for (auto it = current->currentMtnRef->_nodeCache.begin();
                 it != current->currentMtnRef->_nodeCache.end();
                 ++it)
            {
                _drawStack.push_back(&(*it));
            }
        }
    }

    _drawNodes = _drawScratch;
    std::stable_sort(_drawNodes.begin(), _drawNodes.end(),
                     [](emotenoderef* a, emotenoderef* b)
                     { return a->getCurrentRenderZ() < b->getCurrentRenderZ(); });
}

bool emotemotionref::prepareMasks(krkrsdl3::iTVPRenderBackend* renderer, emotelimit lim, void* maskTarget)
{
    // A masked node drawn in place switches to the mask target and back, which
    // ends the canvas render pass and resumes it with a full load and store of
    // the canvas. Instead every mask of the frame is drawn first, each into its
    // own target (the player's mask target, then the engine's extra ones), so
    // the canvas is drawn in one pass. Players call this before clearing their
    // canvas. Mask contents and draw order are unchanged; a frame with more
    // masked nodes than targets draws in place as before.
    _masksPrepared = false;
    size_t masked = 0;
    for (auto r : _drawNodes)
        if (r != nullptr && r->needsMask() && r->canDraw(renderer))
            ++masked;
    if (maskTarget == nullptr || masked == 0 || masked > emoteengine::kMaxPreparedMasks + 1)
        return false;
    std::vector<void*> targets;
    targets.reserve(masked);
    targets.push_back(maskTarget);
    for (size_t extra = 0; targets.size() < masked; ++extra)
    {
        void* target = refTop->maskTargetFor(renderer, extra, int(lim.width), int(lim.height));
        if (target == nullptr)
            return false;
        targets.push_back(target);
    }
    _drawMasks.assign(_drawNodes.size(), nullptr);
    size_t used = 0;
    for (size_t i = 0; i < _drawNodes.size(); ++i)
    {
        auto* r = _drawNodes[i];
        if (r == nullptr || !r->needsMask() || !r->canDraw(renderer))
            continue;
        void* mask = targets[used++];
        if (r->renderMask(renderer, mask, lim))
            _drawMasks[i] = mask;
    }
    _masksPrepared = true;
    return true;
}

void emotemotionref::draw(krkrsdl3::iTVPRenderBackend* renderer, void* target, emotelimit lim, void* maskTarget)
{
    if (!_masksPrepared)
        prepareMasks(renderer, lim, maskTarget);
    if (!_masksPrepared)
    {
        // 绘制
        for (auto r : _drawNodes)
        {
            if (r != nullptr)
            {
                r->wasDrawn = r->draw(renderer, target, lim, maskTarget);
            }
        }
        return;
    }
    // The masks belong to this frame's geometry: one draw consumes them.
    _masksPrepared = false;
    for (size_t i = 0; i < _drawNodes.size(); ++i)
    {
        auto* r = _drawNodes[i];
        if (r != nullptr)
            r->wasDrawn = target && r->canDraw(renderer) && r->drawWithMask(renderer, target, _drawMasks[i]);
    }
}
bool emotemotionref::contains(float x, float y, const char* label) const
{
    for (const auto& area : shapeNodeAreas)
        if ((!label || std::strcmp(area.label.c_str(), label) == 0) && area.contains(x, y))
            return true;
    for (const auto* sub : _subMotionRefs)
        if (sub->contains(x, y, label))
            return true;
    if (!label)
        for (const auto& node : _nodeCache)
            if (node.wasDrawn && node.containsCurrentMesh(x, y))
                return true;
    return false;
}

bool emotemotionref::mayCover(float x, float y, float margin) const
{
    for (const auto* sub : _subMotionRefs)
        if (sub->mayCover(x, y, margin))
            return true;
    for (const auto& node : _nodeCache)
        if (node.wasDrawn && node.nearCurrentMesh(x, y, margin))
            return true;
    return false;
}

bool EmoteHitFrame::mayCoverPixel(int px, int py) const
{
    if (width <= 0 || height <= 0)
        return true; // no frame geometry to reason about
    if (!motion)
        return false; // nothing drawn
    // Rasterizers write a pixel only when its centre is inside a triangle; a
    // one-pixel margin absorbs CPU/GPU rounding of the same geometry.
    const float x = (px + 0.5f) * 2 / width - 1, y = (py + 0.5f) * 2 / height - 1;
    const float margin = std::max(2 / width, 2 / height);
    return motion->mayCover(x, y, margin);
}

bool EmoteHitFrame::contains(const char* label, float x, float y, bool local) const
{
    if (!motion || width <= 0 || height <= 0)
        return false;
    if (local)
    {
        const auto clip = inputToClip * glm::vec4(x, y, 0, 1);
        x = clip.x;
        y = -clip.y; // same final Y flip as evaluateSurfaceChain
    }
    else
    {
        x = x * 2 / width - 1;
        y = y * 2 / height - 1;
    }
    // Geometry outside the rendered target is clipped by all rendering backends.
    return std::isfinite(x) && std::isfinite(y) && x >= -1 && x < 1 && y >= -1 && y < 1 &&
           motion->contains(x, y, label);
}

emoteengine::~emoteengine()
{
    releaseMaskTargets();
}

void* emoteengine::maskTargetFor(krkrsdl3::iTVPRenderBackend* renderer, size_t index, int width, int height)
{
    if (!renderer || index >= kMaxPreparedMasks || width <= 0 || height <= 0)
        return nullptr;
    if (renderer != _maskPoolRenderer || width != _maskPoolWidth || height != _maskPoolHeight)
    {
        releaseMaskTargets();
        _maskPoolRenderer = renderer;
        _maskPoolWidth = width;
        _maskPoolHeight = height;
    }
    while (_maskPool.size() <= index)
    {
        void* target = renderer->CreateTarget(width, height);
        if (!target)
            return nullptr;
        _maskPool.push_back(target);
    }
    return _maskPool[index];
}

void emoteengine::releaseMaskTargets()
{
    if (_maskPoolRenderer && _maskPoolRenderer == krkrsdl3::TVPGetRenderBackend())
        for (void* target : _maskPool)
            _maskPoolRenderer->DestroyTarget(target);
    _maskPool.clear();
    _maskPoolRenderer = nullptr;
    _maskPoolWidth = _maskPoolHeight = 0;
}

void emoteengine::recordAnimationProgress(double milliseconds, double tick, bool mainPlaying)
{
    const bool diagnostics = SDL_GetHintBoolean("MIKAGE_METAL_DIAGNOSTICS", false);
    if (!diagnostics) return;
    ++_animationProgressVersion;
    _animationTrace[(_animationProgressVersion - 1) % _animationTrace.size()] =
        {milliseconds, tick, _animationProgressVersion, _animationDrawVersion, mainPlaying};
    auto& trace = _animationTrace[(_animationProgressVersion - 1) % _animationTrace.size()];
    if (_mainfile && _mainfile->_metadata)
    {
        for (const auto& value : controlVariables)
            if (trace.variableCount < trace.variableValues.size()) trace.variableValues[trace.variableCount++] = value.second;
        for (const auto& playing : currTimeline) if (trace.timelineCount < trace.timelineTimes.size())
            trace.timelineTimes[trace.timelineCount++] = controlTick - playing.startTick;
    }
    const auto now = SDL_GetTicks();
    if (now - _animationLogAt < 1000) return;
    _animationLogAt = now;
    TVPConsoleLog("emote.animation player=%p inputMS=%.3f tick=%.3f mainPlaying=%d progressVersion=%llu drawVersion=%llu drawCalls=%llu repeatedDraws=%llu",
        static_cast<void*>(this), milliseconds, tick, mainPlaying ? 1 : 0,
        static_cast<unsigned long long>(_animationProgressVersion), static_cast<unsigned long long>(_animationDrawVersion),
        static_cast<unsigned long long>(_animationDrawCalls), static_cast<unsigned long long>(_animationRepeatedDraws));
}
void emoteengine::recordAnimationDraw()
{
    if (!SDL_GetHintBoolean("MIKAGE_METAL_DIAGNOSTICS", false)) return;
    ++_animationDrawCalls;
    if (_animationDrawVersion == _animationProgressVersion) ++_animationRepeatedDraws;
    _animationDrawVersion = _animationProgressVersion;
}

void emoteengine::progress(float tick, std::vector<emoteRender>& renderList, emotelimit lim)
{
    if (!_mainmotion)
    {
        _mainMotionRef.reset();
        return;
    }

    // Reuse the runtime tree while the selected motion is unchanged. The node
    // refs own the mesh/vector capacity and are refreshed in place.
    const bool replaced = !_mainMotionRef || _mainMotionRef->currentMotion != _mainmotion;
    if (replaced)
        _mainMotionRef = std::make_shared<emotemotionref>(_mainmotion, this);

    _mainMotionRef->progress(tick, renderList, lim);
    _mainMotionRef->collectDrawNodes();
}
void emoteengine::prepareMasks(krkrsdl3::iTVPRenderBackend* renderer, emotelimit lim, void* maskTarget)
{
    if (_mainMotionRef)
        _mainMotionRef->prepareMasks(renderer, lim, maskTarget);
}
void emoteengine::draw(krkrsdl3::iTVPRenderBackend* renderer, void* target, emotelimit lim, void* maskTarget)
{
    if (_mainMotionRef)
        _mainMotionRef->draw(renderer, target, lim, maskTarget);
}
bool emoteengine::getTickByName(const std::string& name, tjs_real& retVal)
{
    auto it = _varCache.find(name);
    if (it != _varCache.end())
    {
        retVal = it->second;
        return true;
    }
    return false;
}
void emoteengine::addEmoteFile(emotefile* itm)
{
    _attach.push_back(itm);
}
float emoteengine::getZMax()
{
    float _zMaxTmp = 0.0;
    if (_mainfile) _zMaxTmp = _mainfile->getZMax();
    // 附属
    for (auto itm : _attach)
    {
        if (itm->getZMax() > _zMaxTmp)
            _zMaxTmp = itm->getZMax();
    }
    return _zMaxTmp;
}
emotemotion* emoteengine::findmotionByName(const std::string& name)
{
    std::istringstream iss(name);
    std::string token, token1, token2;

    // 源头
    std::getline(iss, token, '/');
    if (strcmp(token.c_str(), "motion") == 0)
    {
        std::getline(iss, token1, '/');
        std::getline(iss, token2, '/');
        // 一级路径
        auto tmp = _mainfile->_objects.find(token1.c_str());
        if (tmp != _mainfile->_objects.end())
        {
            emoteobject* src = tmp->second;
            // 二级路径
            auto tmp1 = src->motion.find(token2.c_str());
            if (tmp1 != src->motion.end())
            {
                return tmp1->second;
            }
        }
        // 附属
        for (auto itm : _attach)
        {
            // 一级路径
            tmp = itm->_objects.find(token1.c_str());
            if (tmp != itm->_objects.end())
            {
                emoteobject* src = tmp->second;
                // 二级路径
                auto tmp1 = src->motion.find(token2.c_str());
                if (tmp1 != src->motion.end())
                {
                    return tmp1->second;
                }
            }
        }
        TVPConsoleLog("motion find failed!!!");
    }
    else
    {
        return nullptr;
    }
    return nullptr;
}
namespace
{
std::string controlKey(const std::string& name)
{
    return name.substr(0, name.find('\0'));
}

bool instantVariable(const emotemetadata& metadata, const std::string& key)
{
    for (const auto& label : metadata._instantVariableList)
        if (controlKey(label) == key) return true;
    return false;
}

bool timelineValue(const emoteTimeVar& variable, double tick, double initial, bool instant, double& value)
{
    const emoteTimeVarFrame* current = nullptr;
    const emoteTimeVarFrame* next = nullptr;
    double previous = initial;
    for (size_t i = 0; i < variable.frameList.size(); ++i)
    {
        const auto* frame = variable.frameList[i];
        if (frame->time > tick) { next = frame; break; }
        if (current && current->hasContent) previous = current->value;
        current = frame;
    }
    if (!current) return false;
    // Terminal empty keys delimit duration; interior empty keys hide the channel.
    if (!current->hasContent && current == variable.frameList.back())
    {
        for (auto it = variable.frameList.rbegin(); it != variable.frameList.rend(); ++it)
            if ((*it)->hasContent) { value = (*it)->value; return true; }
    }
    if (!current->hasContent) return false;
    value = current->value;
    // A timeline key requests a target to reach before the next key. It is
    // different from a geometry key, which describes the pose at its own time.
    if (!instant && next && next->time > current->time + 1)
        value = previous + (value - previous) * emoteEase((tick - current->time) /
                                      (next->time - current->time - 1), current->easing);
    return true;
}
}

void emoteengine::resetControls()
{
    controlTick = 0;
    currTimeline.clear(); baseVariables.clear(); controlVariables.clear(); variableTweens.clear(); _varCache.clear();
    eyes.clear();
    if (!_mainfile || !_mainfile->_metadata) return;
    const auto& metadata = *_mainfile->_metadata;
    for (const auto& entry : metadata._varList) baseVariables[controlKey(entry.first)] = entry.second;
    controlVariables = baseVariables;
    for (const auto* eye : metadata._eyeControl)
    {
        eyes.push_back(*eye);
        auto& copy = eyes.back();
        copy.hasStart = copy.isBlinking = false;
        copy.currWaitInterval = -1;
    }
    physics.reset(metadata._bustControl, metadata._hairControl, metadata._partsControl);
}

bool emoteengine::getControlVariable(const std::string& name, float& value) const
{
    const auto entry = controlVariables.find(controlKey(name));
    if (entry == controlVariables.end()) return false;
    value = entry->second;
    return true;
}
bool emoteengine::getControlVariableByKey(const std::string& key, float& value) const
{
    const auto entry = controlVariables.find(key);
    if (entry == controlVariables.end()) return false;
    value = entry->second;
    return true;
}

void emoteengine::updateEyeControl(float tick, bool isMain)
{
    for (auto& eye : eyes)
    {
        const auto key = controlKey(eye.label);
        if (!eye.hasStart)
        {
            eye.hasStart = true;
            eye.lastTick = tick;
        }
        if (eye.currWaitInterval < 0) eye.currWaitInterval = eye.uid(random);
        if (!eye.isBlinking)
        {
            if (tick - eye.lastTick < eye.currWaitInterval) continue;
            eye.isBlinking = true;
            eye.lastTick = tick;
        }
        const double elapsed = tick - eye.lastTick;
        if (eye.blinkFrameCount <= 0 || elapsed >= eye.blinkFrameCount)
        {
            eye.isBlinking = false; eye.currWaitInterval = -1; eye.lastTick = tick;
            continue;
        }
        const double ratio = 1 - std::abs(2 * elapsed / eye.blinkFrameCount - 1);
        // Compose with this frame's expression, so a closed-eye pose stays closed.
        auto& value = controlVariables[key];
        value += (eye.endFrame - value) * ratio;
    }
}

void emoteengine::startTimeline(float tick, const std::string& name, int flags)
{
    if (!_mainfile || !_mainfile->_metadata) return;
    for (auto* timeline : _mainfile->_metadata->_timelineControl)
    {
        if (controlKey(timeline->label) != controlKey(name)) continue;
        stopTimeline(name);
        PlayingTimeline playing;
        playing.timeline = timeline; playing.startTick = tick;
        playing.flags = flags | 1 | (timeline->diff ? 2 : 0);
        if (!(playing.flags & 2)) playing.initialVariables = baseVariables;
        currTimeline.push_back(playing);
        return;
    }
}

void emoteengine::stopTimeline(const std::string& name, bool isMain)
{
    const auto key = controlKey(name);
    currTimeline.erase(std::remove_if(currTimeline.begin(), currTimeline.end(),
        [&](const PlayingTimeline& playing) { return key.empty() || controlKey(playing.timeline->label) == key; }),
        currTimeline.end());
}

bool emoteengine::checkTimline(const std::string& name, bool& result, bool isMain)
{
    result = false;
    const auto key = controlKey(name);
    for (const auto& playing : currTimeline)
        if (key.empty() || controlKey(playing.timeline->label) == key) { result = true; break; }
    return true;
}

void emoteengine::blendTimeline(const std::string& name, double ratio, double time, double easing, bool stopWhenDone)
{
    for (auto& playing : currTimeline)
        if (controlKey(playing.timeline->label) == controlKey(name))
        {
            playing.blend.set(playing.blend.value(controlTick), ratio, controlTick, time, easing);
            playing.stopWhenBlended = stopWhenDone;
        }
}

void emoteengine::updateTimelineControl(float tick, bool isMain)
{
    for (auto it = variableTweens.begin(); it != variableTweens.end();)
    {
        baseVariables[it->first] = it->second.value(tick);
        if (it->second.done(tick)) it = variableTweens.erase(it);
        else ++it;
    }
    controlVariables = baseVariables;
    // Main channels establish the pose; difference channels add to it. Rebuild
    // from the inputs every frame so a breathing loop never accumulates drift.
    for (int difference = 0; difference < 2; ++difference)
    {
        for (auto& playing : currTimeline)
        {
            if (bool(playing.flags & 2) != bool(difference)) continue;
            auto* timeline = playing.timeline;
            double local = std::max(0.0, tick - playing.startTick);
            if (timeline->loopBegin >= 0 && timeline->loopEnd > timeline->loopBegin && local >= timeline->loopEnd)
                local = timeline->loopBegin + std::fmod(local - timeline->loopBegin,
                                                      timeline->loopEnd - timeline->loopBegin);
            else if (timeline->lastTime >= 0) local = std::min(local, double(timeline->lastTime));
            const double ratio = playing.blend.value(tick);
            for (const auto* variable : timeline->variableList)
            {
                double value;
                const auto initial = playing.initialVariables.find(controlKey(variable->label));
                const bool immediate = instantVariable(*_mainfile->_metadata, controlKey(variable->label));
                if (!timelineValue(*variable, local, initial == playing.initialVariables.end() ? 0 : initial->second, immediate, value)) continue;
                auto& output = controlVariables[controlKey(variable->label)];
                if (difference) output += value * ratio;
                else output += (value - output) * ratio;
            }
        }
    }
    currTimeline.erase(std::remove_if(currTimeline.begin(), currTimeline.end(), [&](const PlayingTimeline& playing)
    {
        const auto* timeline = playing.timeline;
        const bool looping = timeline->loopBegin >= 0 && timeline->loopEnd > timeline->loopBegin;
        const bool finished = !looping && timeline->lastTime >= 0 && tick - playing.startTick >= timeline->lastTime;
        if (finished && !(playing.flags & 2))
            for (const auto* variable : timeline->variableList)
            {
                const auto key = controlKey(variable->label);
                const auto initial = playing.initialVariables.find(key);
                const bool immediate = instantVariable(*_mainfile->_metadata, key);
                double value;
                if (timelineValue(*variable, timeline->lastTime,
                                  initial == playing.initialVariables.end() ? 0 : initial->second, immediate, value))
                    baseVariables[key] += (value - baseVariables[key]) * playing.blend.value(tick);
            }
        return finished || (playing.stopWhenBlended && playing.blend.done(tick));
    }), currTimeline.end());
}

void emoteengine::samplePhysics(float tick, const emoteRender& root, emotelimit lim)
{
    if (physics.empty() || !_mainmotion) return;
    physics.beginAnchors();
    samplingPhysics = true;
    struct Restore { bool& flag; ~Restore() { flag = false; } } restore{samplingPhysics};
    if (!_physicsRef || _physicsRef->currentMotion != _mainmotion)
        _physicsRef = std::make_unique<emotemotionref>(_mainmotion, this);
    _physicsChain.assign(1, root);
    _physicsRef->progress(tick, _physicsChain, lim);
}

bool emoteengine::leadsToPhysicsAnchor(emotenode* node, int depth)
{
    if (node == nullptr || depth > 64) return false; // depth guards motion cycles
    if (physics.needsAnchor(node->label)) return true;
    for (auto* child : node->children)
        if (leadsToPhysicsAnchor(child, depth + 1)) return true;
    for (const auto* frame : node->frameList)
    {
        if (frame == nullptr || !frame->hasContent || node->_filePtr->findsourceByName(frame->src))
            continue;
        if (auto* motion = findmotionByName(frame->src))
            for (auto* inner : motion->nodeList)
                if (leadsToPhysicsAnchor(inner, depth + 1)) return true;
    }
    return false;
}

void emoteengine::recordPhysicsAnchor(const std::string& name, const std::vector<emoteRender>& chain)
{
    if (chain.empty() || !physics.needsAnchor(name)) return;
    float x, y;
    // Shape/point is centered at UV (.5, .5); use the same inherited placement
    // and Bezier evaluation as visible geometry, before camera/projection/affine.
    evaluateSurfaceChain(chain, 0.5f, 0.5f, x, y);
    physics.setAnchor(name, Vector3(x, -y));
}

void emoteengine::updatePhysics(float frames)
{
    physics.update(frames, [&](const std::string& name, double value)
    {
        if (!name.empty() && std::isfinite(value)) controlVariables[controlKey(name)] = value;
    });
}

bool emoteengine::controlsAnimating() const
{
    if (!variableTweens.empty()) return true;
    for (const auto& playing : currTimeline)
        if (!playing.blend.done(controlTick)) return true;
    return false;
}

void emoteengine::skipControls()
{
    for (auto& tween : variableTweens) tween.second.duration = 0;
    for (auto& playing : currTimeline) playing.blend.duration = 0;
    updateTimelineControl(controlTick);
}

emoteVar* emoteengine::findVarByName(const std::string& name)
{
    for (auto obj : _mainfile->_objects)
    {
        emoteVar* ret = nullptr;
        ret = obj.second->findVarByName(name);
        if (ret != nullptr)
            return ret;
    }
    return nullptr;
}
void emoteengine::setVariable(const std::string& name, tjs_real value, double time, double easing)
{
    if (!std::isfinite(value)) return;
    const auto key = controlKey(name);
    if (_mainfile && !_mainfile->isMotion)
    {
        for (const auto* selector : _mainfile->_metadata->_selectorControl)
        {
            if (controlKey(selector->lable) != key) continue;
            const int index = (int)value;
            if (index >= 0 && index < (int)selector->selectItem.size())
                for (size_t i = 0; i < selector->selectItem.size(); ++i)
                {
                    const auto& item = selector->selectItem[i];
                    setVariable(item.label, i == index ? item.onValue : item.offValue, time, easing);
                }
            return;
        }
        const float current = baseVariables.count(key) ? baseVariables[key] : 0;
        if (time > 0) variableTweens[key].set(current, value, controlTick, time, easing);
        else { variableTweens.erase(key); baseVariables[key] = value; controlVariables[key] = value; }
        _varCache[name] = value;
        return;
    }
    // 所有file
    std::vector<emotefile*> allFiles = _attach;
    allFiles.push_back(_mainfile);
    for (auto tmpFile : allFiles)
    {
        // 设置(metadata _varList / _selectorControl)
        tmpFile->setVariable(name, value);

        // motion_inter (name = "motionName/varName")
        size_t pos = name.find('/');
        if (pos != std::string::npos)
        {
            std::string motionName = name.substr(0, name.find('/'));
            emoteobject* obj = nullptr;
            for (auto objItm : _mainfile->_objects)
            {
                if (objItm.first == motionName)
                {
                    obj = objItm.second;
                    break;
                }
            }
            if (obj)
            {
                std::string varName = name.substr(name.find('/') + 1);
                for (auto mtnItm : obj->motion)
                {
                    for (auto varItm : mtnItm.second->parameter)
                    {
                        if (varItm->id == varName)
                        {
                            mtnItm.second->parameterCache[varItm->id] = value;
                            break;
                        }
                    }
                }
}
        }
        else
        {
            // 简单名(如"helptext"): 遍历所有motion的parameter, 匹配id后写入parameterCache
            if (tmpFile != nullptr)
            {
                for (auto& objPair : tmpFile->_objects)
                {
                    for (auto& mtnPair : objPair.second->motion)
                    {
                        for (auto varItm : mtnPair.second->parameter)
                        {
                            if (varItm->id == name)
                            {
                                mtnPair.second->parameterCache[varItm->id] = value;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    // 变量历史缓存
    _varCache[name] = value;
}
tjs_real emoteengine::getVariable(const std::string& name)
{
    const auto base = baseVariables.find(controlKey(name));
    if (base != baseVariables.end()) return base->second;
    float controlled;
    if (getControlVariable(name, controlled)) return controlled;
    size_t pos = name.find('/');
    if (_mainfile->isMotion) // motion类
    {
        size_t pos = name.find('/');
        if (pos != std::string::npos)
        {
            std::string motionName = name.substr(0, name.find('/'));
            emoteobject* obj = nullptr;
            for (auto objItm : _mainfile->_objects)
            {
                if (objItm.first == motionName)
                {
                    obj = objItm.second;
                    break;
                }
            }
            if (obj)
            {
                std::string varName = name.substr(name.find('/') + 1);
                for (auto mtnItm : obj->motion)
                {
                    for (auto varItm : mtnItm.second->parameter)
                    {
                        if (varItm->id == varName)
                        {
                            return mtnItm.second->parameterCache[varItm->id];
                        }
                    }
                }
            }
        }
        else
        {
            // 简单名(如"helptext"): 遍历所有motion的parameter, 匹配id后写入parameterCache
            for (auto& objPair : _mainfile->_objects)
            {
                for (auto& mtnPair : objPair.second->motion)
                {
                    for (auto varItm : mtnPair.second->parameter)
                    {
                        if (varItm->id == name)
                        {
                            return mtnPair.second->parameterCache[varItm->id];
                        }
                    }
                }
            }
        }
    }
    else // emote类
    {
        auto varPos = _mainfile->_metadata->_varList.find(name);
        if (varPos != _mainfile->_metadata->_varList.end())
        {
            return varPos->second;
        }
    }
    // 看看缓存
    tjs_real ret = 0.0;
    if (getTickByName(name, ret))
    {
        return ret;
    }
    return 0.0;
}

} // namespace emoteplayer
