#pragma once

#include <vector>
#include <unordered_map>
#include <list>
#include <memory>
#include <array>
#include <cstdint>
#include <string>

#include "emotefile.h"
#include "emotegeometry.h"
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
    // Shapes are hit areas: every shape node records one per frame, but scripts
    // query them only on touch or layer lookups. The frame's surface chain is
    // kept and the mesh (a 65-point evaluation for circles) is built on first
    // use, from the same inputs the eager build used.
    struct emoterect
    {
        std::string label;
        int shapeType = 2;
        std::vector<emoteRender> methods;
        float viewW = 0, viewH = 0;
        // Set by geometry().
        mutable bool built = false;
        mutable float left = 0, top = 0, width = 0, height = 0;
        mutable std::vector<EmoteVertex> vertices;
        mutable std::vector<uint16_t> indices;
        const emoterect& geometry() const;
        bool contains(float x, float y) const
        {
            geometry();
            return containsMesh(vertices, indices, x, y);
        }
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
        // Whether this node's drawn mesh may have written the pixel at (x, y).
        bool nearCurrentMesh(float x, float y, float margin) const;
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
        // What frame->src names. Resolving it parses the path and walks the
        // source/motion maps; this caches the result for the frame currently
        // shown, keyed by everything the lookup reads (see checkDrawStatus).
        struct SourceResolution
        {
            enum class Kind : uint8_t { Unresolved, Icon, Motion, Layout, Blank, Shape, Unsupported };
            Kind kind = Kind::Unresolved;
            const emoteframe* frame = nullptr;
            std::string src;
            const emotefile* mainFile = nullptr;
            size_t attachCount = 0;
            emoteicon* icon = nullptr;
            emotemotion* motion = nullptr;
            int32_t blankWidth = 0, blankHeight = 0, blankOriginX = 0, blankOriginY = 0;
        } _source;

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
        // Per-node scratch storage survives frames. Index topology is retained
        // until division or mesh mode actually changes.
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
        // Whether any node drawn in the last frame may have written (x, y).
        // Unlike contains, labelled shape areas are ignored: they draw nothing.
        bool mayCover(float x, float y, float margin) const;
        // 根据emotenode*查找对应的emotenoderef
        emotenoderef* getNodeRef(emotenode* node);
        // Available after progress; the list is expanded and sorted only when
        // its membership/order/Z changes, and is shared by draw and bounds scans.
        const std::vector<emotenoderef*>& drawNodes() const { return _drawNodes; }
        void collectDrawNodes();
        void recordShape(const std::string& label, int shapeType,
                         const std::vector<emoteRender>& methods, float viewW, float viewH);

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
        std::vector<emotenoderef*> _drawNodes, _drawScratch, _drawStack;
    };
    // 核心模拟引擎
    class emoteengine
    {
    public:
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
        // 查找数值
        bool getTickByName(const std::string& name, tjs_real& retVal);

        void addEmoteFile(emotefile* itm);
        float getZMax();

        // 控制系列
        emotemotion* findmotionByName(const std::string& name);
        void updateEyeControl(float tick, bool isMain = false);
        struct PlayingTimeline
        {
            emotetimeline* timeline = nullptr;
            double startTick = 0;
            int flags = 0;
            std::map<std::string, float> initialVariables;
            EmoteTween blend{1, 1};
            bool stopWhenBlended = false;
        };
        std::vector<PlayingTimeline> currTimeline;
        double controlTick = 0;
        void resetControls();
        void startTimeline(float tick, const std::string& name, int flags = 0);
        void stopTimeline(const std::string& name, bool isMain = false);
        bool checkTimline(const std::string& name, bool& result, bool isMain = false);
        void updateTimelineControl(float tick, bool isMain = false);
        void blendTimeline(const std::string& name, double ratio, double time, double easing,
                           bool stopWhenDone = false);
        emoteVar* findVarByName(const std::string& name);
        void setVariable(const std::string& name, tjs_real value, double time = 0, double easing = 0);
        tjs_real getVariable(const std::string& name);
        bool getControlVariable(const std::string& name, float& value) const;
        EmotePhysics physics;
        bool samplingPhysics = false;
        void samplePhysics(float tick, const emoteRender& root, emotelimit lim);
        void recordPhysicsAnchor(const std::string& name, const std::vector<emoteRender>& chain);
        void updatePhysics(float frames);
        bool controlsAnimating() const;
        void skipControls();
        tTJSVariant serializeControls() const;
        void unserializeControls(const tTJSVariant& state);

        void recordAnimationProgress(double milliseconds, double tick, bool mainPlaying);
        void recordAnimationDraw();
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

        // Each draw owns its geometry, so shared players can render to multiple layers.
        std::shared_ptr<emotemotionref> _mainMotionRef;
        // 三重签名缓存变量数据
        std::map<std::string, tjs_real> _varCache;
    private:
        std::map<std::string, float> baseVariables, controlVariables;
        std::map<std::string, EmoteTween> variableTweens;
        std::vector<eyeControl> eyes;
        std::default_random_engine random;
    };
    // A snapshot of one draw: queries never rebuild geometry or advance animation.
    struct EmoteHitFrame
    {
        std::shared_ptr<emotemotionref> motion;
        glm::mat4 inputToClip = glm::mat4(1.0f);
        float width = 0, height = 0;
        bool contains(const char* label, float x, float y, bool local = false) const;
        // Whether the last drawn frame may have written target pixel (px, py).
        bool mayCoverPixel(int px, int py) const;
    };
}
