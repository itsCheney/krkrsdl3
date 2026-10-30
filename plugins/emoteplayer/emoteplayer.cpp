#include "ncbind/ncbind.hpp"
#include "emoteplayerclass.h"
#include "tjsString.h"

#define NCB_MODULE_NAME TJS_N("emoteplayer.dll")

using namespace emoteplayer;

#define ENUM(n) Variant(#n, (int)n)
#define PROPERTY(name) NCB_PROPERTY(name, get_##name, set_##name);

namespace
{
tjs_real realArg(tjs_int count, tTJSVariant** args, int index)
{
    return index < count && args[index]->Type() != tvtVoid ? (tjs_real)*args[index] : 0;
}
// The script API has optional transition arguments. Native default arguments do
// not change ncbind's required arity, so dispatch these calls explicitly.
template<class P, int Operation>
tjs_error controlCall(tTJSVariant* result, tjs_int count, tTJSVariant** args, P* player)
{
    if (!player) return TJS_E_FAIL;
    constexpr int required = Operation == 0 || Operation == 3 || Operation == 5 ? 2 :
                             Operation == 4 ? 3 : Operation == 8 || Operation == 9 ? 0 : 1;
    if (count < required) return TJS_E_BADPARAMCOUNT;
    auto number = [&](int index) { return realArg(count, args, index); };
    if constexpr (Operation == 0) player->setCoord(number(0), number(1), number(2), number(3));
    if constexpr (Operation == 1) player->setScale(number(0), number(1), number(2));
    if constexpr (Operation == 2) player->setRotate(number(0), number(1), number(2));
    if constexpr (Operation == 3) player->setVariable(tTJSString(*args[0]), number(1), number(2), number(3));
    if constexpr (Operation == 4) player->setOuterForce(tTJSString(*args[0]), number(1), number(2), number(3), number(4));
    if constexpr (Operation == 5) player->setTimelineBlendRatio(tTJSString(*args[0]), number(1), number(2), number(3), number(4) != 0);
    if constexpr (Operation == 6) player->fadeInTimeline(tTJSString(*args[0]), number(1), number(2));
    if constexpr (Operation == 7) player->fadeOutTimeline(tTJSString(*args[0]), number(1), number(2));
    if constexpr (Operation == 8) player->stopTimeline(count ? tTJSString(*args[0]) : tTJSString());
    if constexpr (Operation == 9)
    {
        const bool playing = player->getTimelinePlaying(count ? tTJSString(*args[0]) : tTJSString());
        if (result) *result = playing;
    }
    if constexpr (Operation == 10) player->playTimeline(tTJSString(*args[0]), (tjs_int)number(1));
    if constexpr (Operation == 11) player->setColor((tjs_uint32)args[0]->AsInteger(), number(1), number(2));
    return TJS_S_OK;
}
// load() returns the file's whole PSB tree as a fresh dictionary, which
// costs about 20 ms for a character. The VM passes no result slot when the
// script discards the value, so that case skips building it.
tjs_error loadCall(tTJSVariant* result, tjs_int count, tTJSVariant** args, ResourceManager* manager)
{
    if (!manager) return TJS_E_FAIL;
    if (count < 1) return TJS_E_BADPARAMCOUNT;
    if (result) *result = manager->load(tTJSString(*args[0]));
    else manager->ensureLoaded(tTJSString(*args[0]));
    return TJS_S_OK;
}
}

#define CONTROL_METHOD(name, type, operation) NCB_METHOD_RAW_CALLBACK(name, (&controlCall<type, operation>), 0)

NCB_REGISTER_SUBCLASS(ResourceManager)
{
    NCB_CONSTRUCTOR((iTJSDispatch2*, tjs_int));
    NCB_METHOD_RAW_CALLBACK(load, &loadCall, 0);
    NCB_METHOD(unload);
    NCB_METHOD(unloadAll);
    NCB_METHOD(clearCache);
    NCB_METHOD(setEmotePSBDecryptSeed);
    NCB_METHOD(setEmotePSBDecryptFunc);
}

NCB_REGISTER_SUBCLASS(SeparateLayerAdaptor)
{
    NCB_CONSTRUCTOR((iTJSDispatch2*));
    NCB_METHOD(assign);
    NCB_METHOD(clear);
    // Layer的所有属性都应该有，NCB机制或许要换成Hook
    PROPERTY(absolute);
    PROPERTY(isPrimary);
    PROPERTY(parent);
}

NCB_REGISTER_SUBCLASS(D3DAdaptor)
{
    // D3DAdaptor就是一种GPU->CPU的回读机制
    // 它不依赖DrawDeviceD3D
    // 现在的emote已经是这种机制了，所以可以直接套壳
    NCB_CONSTRUCTOR((iTJSDispatch2*, tjs_int, tjs_int, tjs_int, tjs_int));
    NCB_METHOD(setClearColor);
    NCB_METHOD(captureCanvas);
    NCB_METHOD(unloadUnusedTextures);
}

NCB_REGISTER_SUBCLASS(EmotePlayer)
{
    NCB_CONSTRUCTOR((ResourceManager*));
    NCB_METHOD(serialize);
    NCB_METHOD(unserialize);
    NCB_METHOD(play);
    NCB_METHOD(initPhysics);
    NCB_METHOD(clear);
    NCB_METHOD(progress);
    NCB_METHOD(draw);
    NCB_METHOD(assign);
    CONTROL_METHOD(setCoord, EmotePlayer, 0);
    CONTROL_METHOD(setScale, EmotePlayer, 1);
    CONTROL_METHOD(setRotate, EmotePlayer, 2);
    CONTROL_METHOD(setColor, EmotePlayer, 11);
    CONTROL_METHOD(setVariable, EmotePlayer, 3);
    NCB_METHOD(getVariable);
    CONTROL_METHOD(setOuterForce, EmotePlayer, 4);
    NCB_METHOD(setDrawAffineTranslateMatrix);
    NCB_METHOD(setCameraOffset);
    NCB_METHOD(startWind);
    NCB_METHOD(stopWind);
    // contains(label, x, y) / contains(x, y) 两参分发（见 EmotePlayer::cb_contains）
    NCB_METHOD_RAW_CALLBACK(contains, &EmotePlayer::cb_contains, 0);
    NCB_METHOD_RAW_CALLBACK(hitTest, &EmotePlayer::cb_hitTest, 0);
    NCB_METHOD(skip);
    NCB_METHOD(skipToSync);
    NCB_METHOD(pass);
    CONTROL_METHOD(playTimeline, EmotePlayer, 10);
    CONTROL_METHOD(stopTimeline, EmotePlayer, 8);
    CONTROL_METHOD(getTimelinePlaying, EmotePlayer, 9);
    NCB_METHOD(getLoopTimeline);
    NCB_METHOD(getTimelineTotalFrameCount);
    NCB_METHOD(getMainTimelineLabelList);
    NCB_METHOD(getDiffTimelineLabelList);
    CONTROL_METHOD(setTimelineBlendRatio, EmotePlayer, 5);
    CONTROL_METHOD(fadeInTimeline, EmotePlayer, 6);
    CONTROL_METHOD(fadeOutTimeline, EmotePlayer, 7);
    NCB_METHOD(getPlayingTimelineInfoList);
    NCB_METHOD(getVariableFrameList);
    PROPERTY(animating);
    PROPERTY(playing);
    PROPERTY(allplaying);
    PROPERTY(loopTime);
    PROPERTY(tickCount);
    PROPERTY(speed);
    PROPERTY(bustScale);
    PROPERTY(hairScale);
    PROPERTY(partsScale);
    PROPERTY(outline);
    PROPERTY(motionKey);
    PROPERTY(motion);
    PROPERTY(chara);
    PROPERTY(variableKeys);
    Variant("TimelinePlayFlagParallel", (int)EmotePlayer::TimelinePlayFlagParallel);
    Variant("TimelinePlayFlagDifference", (int)EmotePlayer::TimelinePlayFlagDifference);
}

NCB_REGISTER_SUBCLASS(Player)
{
    NCB_CONSTRUCTOR((ResourceManager*));
    NCB_METHOD(serialize);
    NCB_METHOD(unserialize);
    NCB_METHOD(play);
    NCB_METHOD(initPhysics);
    NCB_METHOD(clear);
    NCB_METHOD(progress);
    NCB_METHOD(draw);
    NCB_METHOD(assign);
    CONTROL_METHOD(setCoord, Player, 0);
    CONTROL_METHOD(setScale, Player, 1);
    CONTROL_METHOD(setRotate, Player, 2);
    CONTROL_METHOD(setColor, Player, 11);
    CONTROL_METHOD(setVariable, Player, 3);
    NCB_METHOD(getVariable);
    CONTROL_METHOD(setOuterForce, Player, 4);
    NCB_METHOD(setDrawAffineTranslateMatrix);
    NCB_METHOD(setCameraOffset);
    NCB_METHOD(startWind);
    NCB_METHOD(stopWind);
    // contains(label, x, y) / contains(x, y) 两参分发（见 EmotePlayer::cb_contains）
    NCB_METHOD_RAW_CALLBACK(contains, &Player::cb_contains, 0);
    NCB_METHOD_RAW_CALLBACK(hitTest, &Player::cb_hitTest, 0);
    NCB_METHOD(skip);
    NCB_METHOD(skipToSync);
    NCB_METHOD(stop);
    NCB_METHOD(pass);
    CONTROL_METHOD(playTimeline, Player, 10);
    CONTROL_METHOD(stopTimeline, Player, 8);
    CONTROL_METHOD(getTimelinePlaying, Player, 9);
    NCB_METHOD(getLoopTimeline);
    NCB_METHOD(getTimelineTotalFrameCount);
    NCB_METHOD(getMainTimelineLabelList);
    NCB_METHOD(getDiffTimelineLabelList);
    CONTROL_METHOD(setTimelineBlendRatio, Player, 5);
    CONTROL_METHOD(fadeInTimeline, Player, 6);
    CONTROL_METHOD(fadeOutTimeline, Player, 7);
    NCB_METHOD(getPlayingTimelineInfoList);
    NCB_METHOD(getVariableFrameList);
    NCB_METHOD(getCommandList);
    NCB_METHOD(getLayerGetter);
    NCB_METHOD(getLayerMotion);
    NCB_METHOD(setFlip);
    NCB_METHOD(setSlant);
    NCB_METHOD(setZoom);
    PROPERTY(animating);
    PROPERTY(playing);
    PROPERTY(allplaying);
    PROPERTY(loopTime);
    PROPERTY(tickCount);
    PROPERTY(speed);
    PROPERTY(bustScale);
    PROPERTY(hairScale);
    PROPERTY(partsScale);
    PROPERTY(outline);
    PROPERTY(motionKey);
    PROPERTY(motion);
    PROPERTY(chara);
    PROPERTY(variableKeys);
    PROPERTY(tags);
    PROPERTY(useD3D); // 脚本侧判断 D3D emote 可用性（见 AffineSourceMotion）
}

NCB_REGISTER_CLASS(Motion)
{
    NCB_METHOD(getD3DAvailable);
    NCB_PROPERTY(enableD3D, getEnableD3D, setEnableD3D);
    ENUM(MaskModeAlpha);
    ENUM(PlayFlagForce);
    NCB_SUBCLASS(ResourceManager, ResourceManager);
    NCB_SUBCLASS(EmotePlayer, EmotePlayer);
    NCB_SUBCLASS(Player, Player);
    NCB_SUBCLASS(SeparateLayerAdaptor, SeparateLayerAdaptor);
    NCB_SUBCLASS(D3DAdaptor, D3DAdaptor);
}

static void emoteplayer_init()
{
    ResourceManager::ResetSession();
}

static void emoteplayer_done()
{
    ResourceManager::ResetSession();
}

NCB_PRE_REGIST_CALLBACK(emoteplayer_init);
NCB_POST_UNREGIST_CALLBACK(emoteplayer_done);
