#include "tjsCommHead.h"
#include "ncbind/ncbind.hpp"
#include "DrawDeviceD3D.h"
#include "D3DEmotePlayer.h"

#define NCB_MODULE_NAME TJS_N("drawdeviceD3D.dll")

using namespace drawdevice;

//---------------------------------------------------------------------------
// DrawDeviceD3D 脚本类
//---------------------------------------------------------------------------
NCB_REGISTER_CLASS(DrawDeviceD3D)
{
    NCB_CONSTRUCTOR((tjs_int, tjs_int));
    NCB_PROPERTY_RO(interface, getInterface);
    NCB_PROPERTY(width, getWidth, setWidth);
    NCB_PROPERTY(height, getHeight, setHeight);
    NCB_METHOD(setSize);
    NCB_PROPERTY(clearColor, getClearColor, setClearColor);
    NCB_PROPERTY(layerManagerIndex, getLayerManagerIndex, setLayerManagerIndex);
    NCB_PROPERTY(stretchType, getStretchType, setStretchType);
    NCB_PROPERTY(bicubicParam, getBicubicParam, setBicubicParam);
    NCB_PROPERTY(transState, getTransState, setTransState);
    NCB_PROPERTY(maskMode, getMaskMode, setMaskMode);
    NCB_PROPERTY_RO(primaryLayers, getPrimaryLayers);
    NCB_PROPERTY_RO(children, getChildren); // 脚本侧层命中判定依赖该属性枚举 D3D 层
    NCB_METHOD(checkEnable);
    NCB_METHOD(getModule);
    NCB_METHOD(update);
    NCB_METHOD(capture);
    NCB_METHOD(startTransition);
    NCB_METHOD(stopTransition);
    NCB_METHOD(setScreenRect);
    NCB_METHOD(setPrimarySize);
    NCB_METHOD(setOffset);
    NCB_METHOD(recreate);
    NCB_METHOD(finalize);
}

//---------------------------------------------------------------------------
// D3D 脚本类（KAGEnvPlayer.tjs：new D3D(exWidth, exHeight)）
// 与 DrawDeviceD3D 同接口；原生实例为 DrawDeviceD3D 子类，
// D3DLayer/D3DImage 构造经 GetDrawDeviceInstance 接受该对象。
//---------------------------------------------------------------------------
NCB_REGISTER_CLASS(D3D)
{
    NCB_CONSTRUCTOR((tjs_int, tjs_int));
    NCB_PROPERTY_RO(interface, getInterface);
    NCB_PROPERTY(width, getWidth, setWidth);
    NCB_PROPERTY(height, getHeight, setHeight);
    NCB_METHOD(setSize);
    NCB_PROPERTY(clearColor, getClearColor, setClearColor);
    NCB_PROPERTY(layerManagerIndex, getLayerManagerIndex, setLayerManagerIndex);
    NCB_PROPERTY(stretchType, getStretchType, setStretchType);
    NCB_PROPERTY(bicubicParam, getBicubicParam, setBicubicParam);
    NCB_PROPERTY(transState, getTransState, setTransState);
    NCB_PROPERTY(maskMode, getMaskMode, setMaskMode);
    NCB_PROPERTY_RO(primaryLayers, getPrimaryLayers);
    NCB_PROPERTY_RO(children, getChildren); // 脚本侧层命中判定依赖该属性枚举 D3D 层
    NCB_METHOD(checkEnable);
    NCB_METHOD(getModule);
    NCB_METHOD(update);
    NCB_METHOD(capture);
    NCB_METHOD(startTransition);
    NCB_METHOD(stopTransition);
    NCB_METHOD(setScreenRect);
    NCB_METHOD(setPrimarySize);
    NCB_METHOD(setOffset);
    NCB_METHOD(recreate);
    NCB_METHOD(finalize);
}

//---------------------------------------------------------------------------
// 设备实例解析：脚本传入的设备对象可以是 DrawDeviceD3D 或 D3D
//（两者原生实例均为 DrawDeviceD3D 或其子类）
//---------------------------------------------------------------------------
namespace drawdevice
{
DrawDeviceD3D* GetDrawDeviceInstance(iTJSDispatch2* obj)
{
    if (DrawDeviceD3D* dev = ncbInstanceAdaptor<DrawDeviceD3D>::GetNativeInstance(obj))
        return dev;
    if (D3D* d3d = ncbInstanceAdaptor<D3D>::GetNativeInstance(obj))
        return d3d;
    return nullptr;
}
} // namespace drawdevice

//---------------------------------------------------------------------------
// D3DLayer 脚本类
//---------------------------------------------------------------------------
// D3DLayer 工厂：捕获脚本对象（objthis）以便每帧驱动脚本 onUpdate
//（D3DAffineLayer.onUpdate → setMatrix / picture 属性，见 out/data/system/D3D.tjs）
static tjs_error D3DLayerFactory(D3DLayer** result, tjs_int numparams, tTJSVariant** param,
                                 iTJSDispatch2* objthis)
{
    if (numparams < 1 || !param[0] || param[0]->Type() != tvtObject)
        return TJS_E_BADPARAMCOUNT;
    D3DLayer* layer = new D3DLayer(param[0]->AsObjectNoAddRef(), objthis);
    *result = layer;
    return TJS_S_OK;
}
NCB_REGISTER_CLASS(D3DLayer)
{
    RawCallback(D3DLayerFactory);
    NCB_METHOD(setMatrix);
    NCB_METHOD(finalize);
    NCB_PROPERTY(drawPlane, getDrawPlane, setDrawPlane);
    NCB_PROPERTY(frontIndex, getFrontIndex, setFrontIndex);
    NCB_PROPERTY(backIndex, getBackIndex, setBackIndex);
    Variant("DrawPlaneBoth", (int)D3DLayer::DrawPlaneBoth);
    Variant("DrawPlaneFront", (int)D3DLayer::DrawPlaneFront);
    Variant("DrawPlaneBack", (int)D3DLayer::DrawPlaneBack);
}

//---------------------------------------------------------------------------
// D3DImage 脚本类
//---------------------------------------------------------------------------
NCB_REGISTER_CLASS(D3DImage)
{
    NCB_CONSTRUCTOR((iTJSDispatch2*));
    NCB_METHOD(load);
    NCB_METHOD(finalize);
    NCB_PROPERTY_RO(width, getWidth);
    NCB_PROPERTY_RO(height, getHeight);
}

//---------------------------------------------------------------------------
// D3DPicture 脚本类
//---------------------------------------------------------------------------
NCB_REGISTER_CLASS(D3DPicture)
{
    NCB_CONSTRUCTOR((iTJSDispatch2*, iTJSDispatch2*));
    NCB_METHOD(assignImageRange);
    NCB_METHOD(setCoord);
    NCB_METHOD(finalize);
    NCB_PROPERTY(blendMode, getBlendMode, setBlendMode);
    NCB_PROPERTY(opacity, getOpacity, setOpacity);
}

//---------------------------------------------------------------------------
// D3DEmotePlayer 脚本类（包装 emoteplayer 的 GPU 直通播放器）
//---------------------------------------------------------------------------
namespace
{
tjs_real d3dEmoteArg(tjs_int count, tTJSVariant** args, int index)
{
    return index < count && args[index]->Type() != tvtVoid ? (tjs_real)*args[index] : 0;
}
tTJSString d3dEmoteName(tjs_int count, tTJSVariant** args)
{
    return count > 0 && args[0]->Type() != tvtVoid ? tTJSString(*args[0]) : tTJSString();
}
// ncbind requires every native argument, default values included, so calls
// with optional transition arguments are dispatched here (as emoteplayer does).
template<int Operation>
tjs_error d3dEmoteControl(tTJSVariant*, tjs_int count, tTJSVariant** args, D3DEmotePlayer* player)
{
    if (!player) return TJS_E_FAIL;
    constexpr int required = Operation == 0 || Operation == 4 || Operation == 8 ? 2 : Operation == 6 ? 0 : 1;
    if (count < required) return TJS_E_BADPARAMCOUNT;
    auto number = [&](int index) { return d3dEmoteArg(count, args, index); };
    if constexpr (Operation == 0) player->setCoord(number(0), number(1), number(2), number(3));
    if constexpr (Operation == 1) player->setRot(number(0), number(1), number(2));
    if constexpr (Operation == 2) player->setScale(number(0), number(1), number(2));
    if constexpr (Operation == 3) player->setColor((tjs_uint32)args[0]->AsInteger(), number(1), number(2));
    if constexpr (Operation == 4) player->setVariable(d3dEmoteName(count, args), number(1), number(2), number(3));
    if constexpr (Operation == 5) player->playTimeline(d3dEmoteName(count, args), (tjs_int)number(1));
    if constexpr (Operation == 6) player->stopTimeline(d3dEmoteName(count, args));
    if constexpr (Operation == 7) player->fadeOutTimeline(d3dEmoteName(count, args), number(1), number(2));
    if constexpr (Operation == 8)
        player->setTimelineBlendRatio(d3dEmoteName(count, args), number(1), number(2), number(3), (tjs_int)number(4));
    return TJS_S_OK;
}
}

NCB_REGISTER_CLASS(D3DEmotePlayer)
{
    NCB_CONSTRUCTOR((iTJSDispatch2*));
    RawCallback("load", &Class::load, 0); // 可变参数函数
    NCB_METHOD(show);
    NCB_METHOD(clone);
    NCB_METHOD(skip);
    NCB_METHOD(pass);
    NCB_METHOD(progress);
    RawCallback("setCoord", &d3dEmoteControl<0>, 0);
    RawCallback("setRot", &d3dEmoteControl<1>, 0);
    RawCallback("setScale", &d3dEmoteControl<2>, 0);
    RawCallback("setColor", &d3dEmoteControl<3>, 0);
    RawCallback("setVariable", &d3dEmoteControl<4>, 0);
    NCB_METHOD(getVariable);
    // contains(label, x, y)：触摸命中判定，脚本侧对层做触摸检测时调用；
    // RawCallback 以兼容 contains(x, y) 两参形式
    NCB_METHOD_RAW_CALLBACK(contains, &D3DEmotePlayer::cb_contains, 0);
    NCB_METHOD_RAW_CALLBACK(hitTest, &D3DEmotePlayer::cb_hitTest, 0);
    NCB_METHOD(startWind);
    NCB_METHOD(stopWind);
    RawCallback("playTimeline", &d3dEmoteControl<5>, 0);
    RawCallback("stopTimeline", &d3dEmoteControl<6>, 0);
    RawCallback("fadeOutTimeline", &d3dEmoteControl<7>, 0);
    RawCallback("setTimelineBlendRatio", &d3dEmoteControl<8>, 0);
    NCB_METHOD(isTimelinePlaying);
    NCB_METHOD(getTimelineBlendRatio);
    NCB_METHOD(countMainTimelines);
    NCB_METHOD(getMainTimelineLabelAt);
    NCB_METHOD(countDiffTimelines);
    NCB_METHOD(getDiffTimelineLabelAt);
    NCB_METHOD(countPlayingTimelines);
    NCB_METHOD(getPlayingTimelineLabelAt);
    NCB_METHOD(getPlayingTimelineFlagsAt);
    NCB_METHOD(finalize);
    NCB_PROPERTY(animating, getAnimating, setAnimating);
    NCB_PROPERTY(smoothing, getSmoothing, setSmoothing);
    NCB_PROPERTY(meshDivisionRatio, getMeshDivisionRatio, setMeshDivisionRatio);
    NCB_PROPERTY(bustScale, getBustScale, setBustScale);
    NCB_PROPERTY(hairScale, getHairScale, setHairScale);
    NCB_PROPERTY(partsScale, getPartsScale, setPartsScale);
}

//---------------------------------------------------------------------------
// Layer 附加属性：drawPlane / frontIndex / backIndex
//（脚本在 D3D 分支直接给普通 Layer 赋值，见 MainWindow.tjs / BaseLayer.tjs）
//---------------------------------------------------------------------------
NCB_GET_INSTANCE_HOOK(LayerD3DAttach)
{
    NCB_INSTANCE_GETTER(objthis)
    {
        LayerD3DAttach* obj = GetNativeInstance(objthis);
        if (!obj)
        {
            obj = new LayerD3DAttach();
            SetNativeInstance(objthis, obj);
        }
        return obj;
    }
};
NCB_ATTACH_CLASS_WITH_HOOK(LayerD3DAttach, Layer)
{
    NCB_PROPERTY(drawPlane, getDrawPlane, setDrawPlane);
    NCB_PROPERTY(frontIndex, getFrontIndex, setFrontIndex);
    NCB_PROPERTY(backIndex, getBackIndex, setBackIndex);
}

//---------------------------------------------------------------------------
// 插件加载
//---------------------------------------------------------------------------
// 说明：D3D.tjs / D3DAffineSource*.tjs / D3DEmote.tjs 等脚本类由游戏方负责
// 加载（游戏自己的归档内通常自带，或经 KAG 的 KAGLoadScript/Scripts.execStorage
// 回落到系统脚本目录）。这些脚本依赖游戏运行时环境（convertMode、extSourceMap
// 等由游戏的 Initialize.tjs / AffineSourceLayer.tjs 定义），在插件链接时刻
// （patch.tjs 阶段）执行必然失败，因此这里只注册原生类，不自动执行脚本。

void DrawDeviceD3D_init()
{
    try
    {
        // 加载 emoteplayer 插件（D3DEmotePlayer 依赖其 Player 实现）
        ncbAutoRegister::LoadModule(TJS_N("emoteplayer.dll"));
    }
    catch (...)
    {
    }
}

void DrawDeviceD3D_done()
{
}

NCB_PRE_REGIST_CALLBACK(DrawDeviceD3D_init);
NCB_POST_UNREGIST_CALLBACK(DrawDeviceD3D_done);
