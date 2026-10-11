#pragma once
#include "tjsNativeLayer.h"
#include "RenderManager.h"
#include "CPUConsumerTrace.h"

// Own the exact texture, not the Layer's later MainImage. Image replacement or
// exception unwind cannot release a different texture's persistent write lease.
class tTVPScopedLayerPixels : public tTVPScopedTexturePixels {
public:
    tTVPScopedLayerPixels() = default;
    tTVPScopedLayerPixels(iTJSDispatch2* object,bool write,const char* caller="native.layerPixels") { Acquire(object,write,caller); }
    void Acquire(iTJSDispatch2* object,bool write,const char* caller="native.layerPixels") {
        Reset();
        tTJSNI_BaseLayer* layer=nullptr;
        if(!object || TJS_FAILED(object->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
            tTJSNC_Layer::ClassID,reinterpret_cast<iTJSNativeInstance**>(&layer))) || !layer) return;
        krkrsdl3::cpu_consumer_trace::ConsumerScope consumer("Layer.nativePixels",
            write ? krkrsdl3::cpu_consumer_trace::Access::Write : krkrsdl3::cpu_consumer_trace::Access::Read,
            "ScopedLayerPixels.Acquire",reinterpret_cast<uintptr_t>(object),true);
        krkrsdl3::layer_work::SourceScope scope(caller);
        tTVPScopedTexturePixels::Acquire(layer->GetMainImageTextureForCPUAccess(write),write,caller);
    }
};
