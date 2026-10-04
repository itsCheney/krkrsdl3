#pragma once

#include <vector>
#include <unordered_map>
#include <list>
#include <memory>
#include <array>

#include "emotefile.h"
#include "emotegeometry.h"
#include "emoteanimation.h"
#include "emoteperformance.h"

namespace emoteplayer
{
    class emotenoderef;
    class emotemotionref;
    class emoteengine;

    struct emotelimit // 区域限制
    {
        float originX = 0.0f;
        float originY = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float zMax = 0.0f;
        // 实际目标尺寸；用于脚本边界属性与画布坐标入口。0 回退到 width/height。
        float viewW = 0.0f;
        float viewH = 0.0f;
    };
    // Shape bounds are exposed to scripts; containment always uses the NDC mesh.
    struct emoterect
    {
        std::string label;
        float left = 0, top = 0, width = 0, height = 0;
        int shapeType = 2;
        std::vector<EmoteVertex> vertices;
        std::vector<uint16_t> indices;
        bool contains(float x, float y) const { return containsMesh(vertices, indices, x, y); }
    };
    // 引用自绘制 - 每次绘制创建独立ref，持有node引用+独立状态，避免多次绘制状态重复
    class emotenoderef
    {
    public:
        emotenoderef(emotenode* en, emoteengine* ee, emotemotionref* em) : currentNode(en), refTop(ee), refMtn(em) {};

        // method
        void checkDrawStatus(float tick, std::vector<emoteRender>& renderList, emotelimit lim);
        void progress(float tick, std::vector<emoteRender>& renderList, emotelimit lim);
        bool containsCurrentMesh(float x, float y) const;
        // 通过 core/render 的 2D 渲染抽象绘制（插件无渲染后端区分）
        bool draw(krkrsdl3::iTVPRenderBackend* renderer, void* target, emotelimit lim, void* maskTarget);
        float getCurrentRenderZ();

        // ref
        emotenode* currentNode = nullptr;
        emotemotion* currentMtn = nullptr;
        emotemotionref* currentMtnRef = nullptr;
        emoteengine* refTop = nullptr;
        emotemotionref* refMtn = nullptr;

        // render method
        std::vector<emoteRender> renderMethod;

        // check
        emoteicon* ic = nullptr;
        float originX = 0;
        float originY = 0;
        float width = 0;
        float height = 0;
        bool isNeedDraw = false;
        bool isIcon = false;
        bool wasDrawn = false; // only meshes submitted to the visible target can be hit
        bool isLayout = false;
        // shape 判定层（触摸判定用）：src 以 "shape/" 开头，
        // 或 src 缺省（部分导出的 PSB 剥离了 shape 帧的 src 字段，
        // 解析后落到 layout）且 node type==1
        bool isShape = false;
        emoteframe* frame = nullptr;
        emoteframe* nextframe = nullptr;

        // runtime (独立状态量，避免多次绘制串扰)
        float currTick = 0;
        int8_t currbm = 0;
        float currCoordx = 0;
        float currCoordy = 0;
        float currCoordz = 0;
        float currOpa = 1.0;
        float currAngle = 0.0;
        float currSx = 0.0, currSy = 0.0;
        float currZx = 0.0, currZy = 0.0;
        float currOx = 0.0, currOy = 0.0;
        float currTimeOffset = 0.0;
        bool isNeedBp = false;
        float currbp[32] = {0.0};

        // CPU subdivision mesh data
        using MeshVertex = EmoteVertex;
        int _meshDivX = 8;
        int _meshDivY = 8;
        std::vector<MeshVertex> _meshVertices;
        std::vector<uint16_t> _meshIndices;
        bool _useGPUDeform = false;
        std::vector<krkrsdl3::TVPMeshDeformSurface> _gpuDeformSurfaces;
        // Per-node scratch storage survives frames. Surface matrices are rebuilt
        // once per mesh update, while index topology is retained until division
        // or mesh mode actually changes.
        std::vector<glm::mat4> _surfaceMatrices;
        // Prepared geometry belongs to this node instance. The comparison key
        // includes the complete inherited chain; equal local poses alone do not
        // permit reusing geometry after a parent or viewport transform changes.
        std::vector<emoteRender> _geometryMethods;
        // Selected-frame scalars are cached separately from inherited geometry.
        // Values (not just frame pointers) are compared so explicit resource
        // edits invalidate the cache, and viewport sentinel resolution is local.
        std::array<double, 101> _localPoseInputs{};
        bool _localPoseValid = false;
        emotelimit _geometryLimit;
        emoterect _shapeArea;
        std::uint64_t _geometryRevision = 0;
        bool _geometryValid = false, _geometryGPUCapable = false;
        bool _geometryDrawable = false, _geometryIcon = false, _geometryShape = false;
        bool _geometryRemoved = false, _geometryHasColor = false;
        int _geometryDivision = 0, _geometryBlend = 0;
        std::int64_t _geometryColor = 0;
        emoteicon* _geometrySource = nullptr;
        void* _geometryTexture = nullptr;
        emotemotion* _geometryMotion = nullptr;
        std::string _geometryFrameSource;
    };
    // motion辅助类 - 管理按priority排序的nodeList并处理子motion展开
    class emotemotionref
    {
    public:
        emotemotionref(emotemotion* mt, emoteengine* ee, emotenoderef* en = nullptr)
          : currentMotion(mt),
            refTop(ee),
            parent(en),
            label(en ? en->currentNode->label : "") {};
        ~emotemotionref();

        float getTickByIdx(int32_t parameterIdx);
        void progress(float tick, std::vector<emoteRender>& renderList, emotelimit lim);
        void draw(krkrsdl3::iTVPRenderBackend* renderer, void* target, emotelimit lim, void* maskTarget);
        bool contains(float x, float y, const char* label = nullptr) const;
        // 根据emotenode*查找对应的emotenoderef
        emotenoderef* getNodeRef(emotenode* node);
        // Available after progress; the list is expanded and sorted only when
        // its membership/order/Z changes, and is shared by draw and bounds scans.
        const std::vector<emotenoderef*>& drawNodes() const { return _drawNodes; }
        void collectDrawNodes();
        void recordShape(const emoterect& area);
        std::uint64_t poseRevision() const { return _poseRevision; }

        emotemotion* currentMotion = nullptr;
        emoteengine* refTop = nullptr;
        std::vector<emoteRender> renderMethod;
        emotenoderef* parent = nullptr;
        std::string label; // retain the instance label in the drawn snapshot

        // 核心: 按priority排序的ref列表，平行于currentMotion->nodeList
        std::vector<emotenoderef> _nodeCache;
        std::unordered_map<emotenode*, size_t> _nodeIndex;
        // 子motion引用缓存(progress阶段创建，draw阶段使用)
        std::vector<emotemotionref*> _subMotionRefs;
        // Sub-motion refs retained across frames, keyed by the emotemotion they
        // were built for. The active set is rebuilt every frame into
        // _subMotionRefs, but the objects themselves are reused so a steady
        // animation stops churning the heap. Owns everything it holds.
        std::unordered_map<emotemotion*, std::vector<std::unique_ptr<emotemotionref>>> _subMotionPool;
        // Per-frame high-water mark of pool entries handed out per motion.
        std::unordered_map<emotemotion*, size_t> _subMotionUsed;

        // shape节点区域(用于 getLayerGetter/getLayerMotion 的shape返回和contains检测)
        std::vector<emoterect> shapeNodeAreas;
        size_t _shapeCount = 0;
        bool _poseChanged = false;
        std::uint64_t _poseRevision = 0;
        std::vector<emotemotionref*> _previousSubMotions;
        std::vector<std::uint64_t> _previousSubRevisions;
        std::vector<emotenoderef*> _drawNodes, _drawInput, _drawScratch, _drawStack;
        std::vector<float> _drawZ;
    };
    // 核心模拟引擎
    class emoteengine
    {
    public:
        emoteengine();
	    // 主file/motion
        emotefile* _mainfile = nullptr;
        emotemotion* _mainmotion = nullptr;
	    // 附属file
        std::vector<emotefile*> _attach;
        float _zMax = 0.0f;

        ~emoteengine();

        // progress/draw接口(替代_mainMotionRef)
        void progress(float tick, std::vector<emoteRender>& renderList, emotelimit lim);
        void draw(krkrsdl3::iTVPRenderBackend* renderer, void* target, emotelimit lim, void* maskTarget);
        // A monotonic prepared-content generation, independent of diagnostics
        // and clock-call counters. Never resets during the engine lifetime.
        std::uint64_t poseRevision() const { return _poseRevision; }
        bool nodeCachingEnabled() const { return _nodeCachingEnabled; }
        bool localPoseCachingEnabled() const { return _localPoseCachingEnabled; }
        bool contentTrackingEnabled() const { return _contentTrackingEnabled; }
        // 查找数值
        bool getTickByName(const std::string& name, tjs_real& retVal);

        void addEmoteFile(emotefile* itm);
        float getZMax();

        // 控制系列
        emotemotion* findmotionByName(const std::string& name);
        void updateEyeControl(float tick, bool isMain = false);
        std::vector<emotetimeline*> currTimeline;
        float currStartTick = -1.0f;
        void startTimeline(float tick, const std::string& name, bool isMain = false, int flags = 0);
        void stopTimeline(const std::string& name, bool isMain = false);
        bool checkTimline(const std::string& name, bool& result, bool isMain = false);
        void updateTimelineControl(float tick, bool isMain = false);
        emoteVar* findVarByName(const std::string& name);
        void setVariable(const std::string& name, tjs_real value);
        tjs_real getVariable(const std::string& name);
        void updatePhysics(float tick);

        bool integratedAnimation() const { return _integratedAnimation; }
        void inheritAnimationMode(bool value) { _integratedAnimation = value; resetAnimationState(); }
        void ensureAnimationState();
        void resetAnimationState();
        void advanceAnimation(double milliseconds, double speedDivisor);
        void setAnimationVariable(const std::string& name, double value, double time, double easing);
        void updateAnimationSelectors();
        void recordAnimationProgress(double milliseconds, double tick, bool mainPlaying);
        void recordAnimationDraw();
        void copyAnimationStateFrom(const emoteengine& source);
        std::string serializeAnimationState() const;
        bool restoreAnimationState(const std::string& text);
        bool getMotionParameter(emotemotion* motion, const std::string& id, double& value);
        animation::Runtime _animation;
        double _animationClock = 0;
        bool _animationPaused = false;
        // This pointer identifies a binding; the ResourceManager owns its lifetime.
        emotefile* _animationFile = nullptr;
        std::vector<eyeControl> _animationEyes;
        std::vector<eyeControl*> _animationEyeRefs;
        std::map<emotemotion*, std::map<std::string, std::string>> _animationParameterLabels;
        struct AnimationTrace {
            double inputMS = 0, tick = 0;
            std::uint64_t progressVersion = 0, drawVersion = 0;
            bool mainPlaying = false;
            std::array<double, 8> variableValues{}, timelineTimes{};
            std::uint8_t variableCount = 0, timelineCount = 0;
        };
        std::array<AnimationTrace, 128> _animationTrace{};
        std::uint64_t _animationProgressVersion = 0, _animationDrawVersion = 0;
        std::uint64_t _animationDrawCalls = 0, _animationRepeatedDraws = 0;
        std::uint64_t _animationLogAt = 0;
    private:
        bool _integratedAnimation = false;
        bool _nodeCachingEnabled = false;
        bool _localPoseCachingEnabled = false;
        bool _contentTrackingEnabled = false;
        std::uint64_t _poseRevision = 0, _preparedMotionRevision = 0;
    public:

        // Each draw owns its geometry, so shared players can render to multiple layers.
        std::shared_ptr<emotemotionref> _mainMotionRef;
        // 三重签名缓存变量数据
        std::map<std::string, tjs_real> _varCache;
    };
    // A snapshot of one draw: queries never rebuild geometry or advance animation.
    struct EmoteHitFrame
    {
        std::shared_ptr<emotemotionref> motion;
        glm::mat4 inputToClip = glm::mat4(1.0f);
        float width = 0, height = 0;
        bool contains(const char* label, float x, float y, bool local = false) const;
    };
}
