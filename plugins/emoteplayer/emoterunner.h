#pragma once

#include <vector>
#include <list>
#include <memory>

#include "emotefile.h"
#include "emotegeometry.h"

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

        emotemotion* currentMotion = nullptr;
        emoteengine* refTop = nullptr;
        std::vector<emoteRender> renderMethod;
        emotenoderef* parent = nullptr;
        std::string label; // retain the instance label in the drawn snapshot

        // 核心: 按priority排序的ref列表，平行于currentMotion->nodeList
        std::vector<emotenoderef> _nodeCache;
        // 子motion引用缓存(progress阶段创建，draw阶段使用)
        std::vector<emotemotionref*> _subMotionRefs;

        // shape节点区域(用于 getLayerGetter/getLayerMotion 的shape返回和contains检测)
        std::vector<emoterect> shapeNodeAreas;
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
        // 查找数值
        bool getTickByName(const std::string& name, tjs_real& retVal);

        void addEmoteFile(emotefile* itm);
        float getZMax();

        // 控制系列
        emotemotion* findmotionByName(const std::string& name);
        void updateEyeControl(float tick, bool isMain = false);
        std::vector<emotetimeline*> currTimeline;
        float currStartTick = -1.0f;
        void startTimeline(float tick, const std::string& name, bool isMain = false);
        void stopTimeline(const std::string& name, bool isMain = false);
        bool checkTimline(const std::string& name, bool& result, bool isMain = false);
        void updateTimelineControl(float tick, bool isMain = false);
        emoteVar* findVarByName(const std::string& name);
        void setVariable(const std::string& name, tjs_real value);
        tjs_real getVariable(const std::string& name);
        void updatePhysics(float tick);

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
