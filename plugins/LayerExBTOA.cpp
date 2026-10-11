#include "ncbind/ncbind.hpp"
#include <vector>
#include "ScopedLayerPixels.h"
#include "MetalLayerRenderManager.h"

#define NCB_MODULE_NAME TJS_N("layerExBTOA.dll")

// レイヤクラスを参照
iTJSDispatch2* getLayerClass(void)
{
    tTJSVariant var;
    TVPExecuteExpression(TJS_N("Layer"), &var);
    return var.AsObjectNoAddRef();
}

//----------------------------------------------
// レイヤイメージ操作ユーティリティ

// バッファ参照用の型
typedef unsigned char* WrtRefT;
typedef unsigned char const* ReadRefT;

static tTJSNI_BaseLayer* NativeLayer(iTJSDispatch2* object)
{
    tTJSNI_BaseLayer* layer=nullptr;
    if(!object || TJS_FAILED(object->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
        tTJSNC_Layer::ClassID,reinterpret_cast<iTJSNativeInstance**>(&layer)))) return nullptr;
    return layer;
}
// Preserve COW and raw script pointers. Metal's normal operator routing still
// provides a software fallback if the backend cannot execute a mask operation.
static bool OperateGPUMask(iTJSDispatch2* object,const char* methodName,const tTVPRect& destination,
                          iTJSDispatch2* sourceObject=nullptr,const tTVPRect& source=tTVPRect(),int value=255)
{
    if(!TVPMetalLayerCompositionActive()) return false;
    auto* layer=NativeLayer(object);
    auto* target=layer ? layer->GetMainImageTextureForCPUAccess(false) : nullptr;
    if(!target || target->IsCPUResident() || target->GetFormat()!=TVPTextureFormat::RGBA) return false;
    auto* sourceLayer=sourceObject ? NativeLayer(sourceObject) : nullptr;
    auto* input=sourceLayer ? sourceLayer->GetMainImageTextureForCPUAccess(false) : nullptr;
    if(sourceObject && (!input || input->GetFormat()!=TVPTextureFormat::RGBA)) return false;
    // Offset alpha aliases follow CPU scanline order. Defer before COW, so a
    // shared old source retains the same semantics as the native pixel path.
    if(sourceLayer==layer && !std::strcmp(methodName,"MultiplyAlpha") && destination!=source) return false;
    target=layer->GetMainImageTextureForCPUAccess(true);
    // A self-layer source must follow COW to the writable epoch.
    if(sourceLayer==layer) input=target;
    auto* manager=TVPGetRenderManager(); auto* method=manager->GetRenderMethod(methodName);
    method->SetParameterOpa(method->EnumParameterID("opacity"),value);
    tRenderTexRectArray::Element texture(input,source);
    manager->OperateRect(method,target,nullptr,destination,tRenderTexRectArray(input ? &texture : nullptr,input ? 1 : 0));
    return true;
}

/**
 * レイヤのサイズとバッファを取得する
 */
static bool GetLayerSize(iTJSDispatch2* lay, long& w, long& h, long& pitch)
{
    iTJSDispatch2* layerClass = getLayerClass();

    // レイヤインスタンス以外ではエラー
    if (!lay || TJS_FAILED(lay->IsInstanceOf(0, 0, 0, TJS_N("Layer"), lay)))
        return false;

    // レイヤイメージは在るか？
    tTJSVariant val;
    if (TJS_FAILED(layerClass->PropGet(0, TJS_N("hasImage"), 0, &val, lay)) ||
        (val.AsInteger() == 0))
        return false;

    // レイヤサイズを取得
    val.Clear();
    if (TJS_FAILED(layerClass->PropGet(0, TJS_N("imageWidth"), 0, &val, lay)))
        return false;
    w = (long)val.AsInteger();

    val.Clear();
    if (TJS_FAILED(layerClass->PropGet(0, TJS_N("imageHeight"), 0, &val, lay)))
        return false;
    h = (long)val.AsInteger();

    // ピッチ取得
    val.Clear();
    if (TJS_FAILED(layerClass->PropGet(0, TJS_N("mainImageBufferPitch"), 0, &val, lay)))
        return false;
    pitch = (long)val.AsInteger();

    // 正常な値かどうか
    return (w > 0 && h > 0 && pitch != 0);
}

// 書き込み用
static bool GetLayerBufferAndSize(iTJSDispatch2* lay, long& w, long& h, WrtRefT& ptr, long& pitch, tTVPScopedLayerPixels& access)
{
    iTJSDispatch2* layerClass = getLayerClass();

    if (!GetLayerSize(lay, w, h, pitch))
        return false;

    access.Acquire(lay,true,krkrsdl3::layer_work::source);
    ptr=static_cast<WrtRefT>(access.Data()); pitch=access.Pitch();
    access.Written(tTVPRect(0,0,w,h));
    return ptr!=nullptr;
}

/**
 * Layer.copyRightBlueToLeftAlpha
 * レイヤ右半分の Blue CHANNEL を左半分の Alpha CHANNEL に複製する
 */
static tjs_error copyRightBlueToLeftAlpha(tTJSVariant* result,
                                          tjs_int numparams,
                                          tTJSVariant** param,
                                          iTJSDispatch2* lay)
{
    krkrsdl3::layer_work::SourceScope origin("layerExBTOA.rightBlue");
    long width,height,pitch;
    if(GetLayerSize(lay,width,height,pitch) && OperateGPUMask(lay,"CopyBlueToAlpha",
        tTVPRect(0,0,width/2,height),lay,tTVPRect(width/2,0,width/2+width/2,height))) return TJS_S_OK;
    // 書き込み先
    tTVPScopedLayerPixels access;
    WrtRefT dbuf = 0;
    long dw, dh, dpitch;
    if (!GetLayerBufferAndSize(lay, dw, dh, dbuf, dpitch, access))
    {
        TVPThrowExceptionMessage(TJS_N("dest must be Layer."));
    }

    // 半分
    dw /= 2;
    access.Written(tTVPRect(0,0,dw,dh));
    // コピー

    WrtRefT sbuf = dbuf + dw * 4;
    dbuf += 3;
    for (int i = 0; i < dh; i++)
    {
        WrtRefT p = sbuf; // B領域
        WrtRefT q = dbuf; // A領域
        for (int j = 0; j < dw; j++)
        {
            *q = *p;
            p += 4;
            q += 4;
        }
        sbuf += dpitch;
        dbuf += dpitch;
    }
    return TJS_S_OK;
}

/**
 * Layer.copyBottomBlueToTopAlpha
 * レイヤ右半分の Blue CHANNEL を左半分の Alpha CHANNELに複製する
 */
static tjs_error copyBottomBlueToTopAlpha(tTJSVariant* result,
                                          tjs_int numparams,
                                          tTJSVariant** param,
                                          iTJSDispatch2* lay)
{
    krkrsdl3::layer_work::SourceScope origin("layerExBTOA.bottomBlue");
    long width,height,pitch;
    if(GetLayerSize(lay,width,height,pitch) && OperateGPUMask(lay,"CopyBlueToAlpha",
        tTVPRect(0,0,width,height/2),lay,tTVPRect(0,height/2,width,height/2+height/2))) return TJS_S_OK;
    // 書き込み先
    tTVPScopedLayerPixels access;
    WrtRefT dbuf = 0;
    long dw, dh, dpitch;
    if (!GetLayerBufferAndSize(lay, dw, dh, dbuf, dpitch, access))
    {
        TVPThrowExceptionMessage(TJS_N("dest must be Layer."));
    }

    // 半分
    dh /= 2;
    access.Written(tTVPRect(0,0,dw,dh));

    // コピー
    WrtRefT sbuf = dbuf + dh * dpitch;
    dbuf += 3;
    for (int i = 0; i < dh; i++)
    {
        WrtRefT p = sbuf; // B領域
        WrtRefT q = dbuf; // A領域
        for (int j = 0; j < dw; j++)
        {
            *q = *p;
            p += 4;
            q += 4;
        }
        sbuf += dpitch;
        dbuf += dpitch;
    }
    return TJS_S_OK;
}

static tjs_error fillAlpha(tTJSVariant* result,
                           tjs_int numparams,
                           tTJSVariant** param,
                           iTJSDispatch2* lay)
{
    krkrsdl3::layer_work::SourceScope origin("layerExBTOA.fillAlpha");
    long width,height,pitch;
    if(GetLayerSize(lay,width,height,pitch) && OperateGPUMask(lay,"FillMask",tTVPRect(0,0,width,height))) return TJS_S_OK;
    // 書き込み先
    tTVPScopedLayerPixels access;
    WrtRefT dbuf = 0;
    long dw, dh, dpitch;
    if (!GetLayerBufferAndSize(lay, dw, dh, dbuf, dpitch, access))
    {
        TVPThrowExceptionMessage(TJS_N("dest must be Layer."));
    }
    // 全部 0xffでうめる
    dbuf += 3;
    for (int i = 0; i < dh; i++)
    {
        WrtRefT q = dbuf; // A領域
        for (int j = 0; j < dw; j++)
        {
            *q = 0xff;
            q += 4;
        }
        dbuf += dpitch;
    }
    return TJS_S_OK;
}

static tjs_error copyAlphaToProvince(tTJSVariant* result,
                                     tjs_int numparams,
                                     tTJSVariant** param,
                                     iTJSDispatch2* lay)
{
    krkrsdl3::cpu_consumer_trace::ConsumerScope consumer("copyAlphaToProvince",
        krkrsdl3::cpu_consumer_trace::Access::Read,"LayerExBTOA.copyAlphaToProvince",reinterpret_cast<uintptr_t>(lay));
    iTJSDispatch2* layerClass = getLayerClass();

    ReadRefT sbuf = 0;
    WrtRefT dbuf = 0;
    long w, h, spitch, dpitch, threshold = -1;
    if (numparams > 0 && param[0]->Type() != tvtVoid)
    {
        threshold = (long)(param[0]->AsInteger());
    }

    if (!GetLayerSize(lay, w, h, spitch))
    {
        TVPThrowExceptionMessage(TJS_N("src must be Layer."));
    }

    tTJSVariant val;
    tTVPScopedLayerPixels sourceAccess(lay,false,"layerExBTOA.alphaToProvince");
    if ((sbuf = static_cast<ReadRefT>(sourceAccess.Data())) == NULL)
    {
        TVPThrowExceptionMessage(TJS_N("src has no image."));
    }

    val.Clear();
    if (TJS_FAILED(layerClass->PropGet(0, TJS_N("provinceImageBufferForWrite"), 0, &val, lay)) ||
        (dbuf = reinterpret_cast<WrtRefT>(val.AsInteger())) == NULL)
    {
        TVPThrowExceptionMessage(TJS_N("dst has no province image."));
    }
    val.Clear();
    if (TJS_FAILED(layerClass->PropGet(0, TJS_N("provinceImageBufferPitch"), 0, &val, lay)) ||
        (dpitch = (long)val.AsInteger()) == 0)
    {
        TVPThrowExceptionMessage(TJS_N("dst has no province pitch."));
    }

    sbuf += 3;
    unsigned char th = (unsigned char)threshold;
    int mode = 0;
    if (threshold >= 0 && threshold < 256)
        mode = 1;
    else if (threshold >= 256)
        mode = 2;

    for (int y = 0; y < h; y++)
    {
        WrtRefT p = dbuf;
        ReadRefT q = sbuf;
        switch (mode)
        {
            case 0:
                for (int x = 0; x < w; x++, q += 4)
                    *p++ = *q;
                break;
            case 1:
                for (int x = 0; x < w; x++, q += 4)
                    *p++ = (*q >= th);
                break;
            case 2:
                for (int x = 0; x < w; x++, q += 4)
                    *p++ = 0;
                break;
        }
        sbuf += spitch;
        dbuf += dpitch;
    }
    return TJS_S_OK;
}

static tjs_error clipAlphaRect(tTJSVariant* result,
                               tjs_int numparams,
                               tTJSVariant** param,
                               iTJSDispatch2* dst)
{
    iTJSDispatch2* layerClass = getLayerClass();

    tTVPScopedLayerPixels sourceAccess, destinationAccess;
    ReadRefT sbuf = 0;
    WrtRefT dbuf = 0;
    iTJSDispatch2* src = 0;
    tTJSVariant val;
    long w, h;
    long dx, dy, diw, dih, dpitch;
    long sx, sy, siw, sih, spitch;
    unsigned char clrval = 0;
    bool clr = false;
    if (numparams < 7)
        return TJS_E_BADPARAMCOUNT;

    dx = (long)param[0]->AsInteger();
    dy = (long)param[1]->AsInteger();
    src = param[2]->AsObjectNoAddRef();
    sx = (long)param[3]->AsInteger();
    sy = (long)param[4]->AsInteger();
    w = (long)param[5]->AsInteger();
    h = (long)param[6]->AsInteger();
    if (numparams >= 8 && param[7]->Type() != tvtVoid)
    {
        long n = (long)param[7]->AsInteger();
        clr = (n >= 0 && n < 256);
        clrval = (unsigned char)(n & 255);
    }
    if (w <= 0 || h <= 0)
        return TJS_E_INVALIDPARAM;

    if (!GetLayerSize(dst, diw, dih, dpitch))
    {
        TVPThrowExceptionMessage(TJS_N("dest must be Layer."));
    }
    if (!GetLayerSize(src, siw, sih, spitch))
    {
        TVPThrowExceptionMessage(TJS_N("src must be Layer."));
    }

    // クリッピング

    // srcが範囲外
    if (sx + w <= 0 || sy + h <= 0 || sx >= siw || sy >= sih)
        goto none;

    // srcの負方向のカット
    if (sx < 0)
    {
        w += sx;
        dx -= sx;
        sx = 0;
    }
    if (sy < 0)
    {
        h += sy;
        dy -= sy;
        sy = 0;
    }

    // srcの正方向のカット
    long cut;
    if ((cut = sx + w - siw) > 0)
        w -= cut;
    if ((cut = sy + h - sih) > 0)
        h -= cut;

    // dstが範囲外
    if (dx + w <= 0 || dy + h <= 0 || dx >= diw || dy >= dih)
        goto none;

    // dstの負方向のカット
    if (dx < 0)
    {
        w += dx;
        sx -= dx;
        dx = 0;
    }
    if (dy < 0)
    {
        h += dy;
        sy -= dy;
        dy = 0;
    }

    // dstの正方向のカット
    if ((cut = dx + w - diw) > 0)
        w -= cut;
    if ((cut = dy + h - dih) > 0)
        h -= cut;

    if (w <= 0 || h <= 0)
        goto none;

    if(OperateGPUMask(dst,"MultiplyAlpha",tTVPRect(dx,dy,dx+w,dy+h),src,tTVPRect(sx,sy,sx+w,sy+h))) {
        if(clr) {
            for(const auto& region:{tTVPRect(0,0,diw,dy),tTVPRect(0,dy+h,diw,dih),
                tTVPRect(0,dy,dx,dy+h),tTVPRect(dx+w,dy,diw,dy+h)})
                if(region.get_width()>0 && region.get_height()>0) OperateGPUMask(dst,"FillMask",region,nullptr,tTVPRect(),clrval);
        }
        return TJS_S_OK;
    }

    sourceAccess.Acquire(src,false,"layerExBTOA.clipAlpha.read");
    destinationAccess.Acquire(dst,true,"layerExBTOA.clipAlpha.write");
    sbuf=static_cast<ReadRefT>(sourceAccess.Data()); spitch=sourceAccess.Pitch();
    dbuf=static_cast<WrtRefT>(destinationAccess.Data()); dpitch=destinationAccess.Pitch();
    destinationAccess.Written(clr ? tTVPRect(0,0,diw,dih) : tTVPRect(dx,dy,dx+w,dy+h));

    if (!sbuf || !dbuf)
        TVPThrowExceptionMessage(TJS_N("Layer has no images."));

    long x, y;
    WrtRefT p;
    ReadRefT q;
    if (clr)
    {
        for (y = 0; y < dy; y++)
            for ((x = 0, p = dbuf + y * dpitch + 3); x < diw; x++, p += 4)
                *p = clrval;
        for (y = dy + h; y < dih; y++)
            for ((x = 0, p = dbuf + y * dpitch + 3); x < diw; x++, p += 4)
                *p = clrval;
    }
    for (y = 0; y < h; y++)
    {
        if (clr)
            for ((x = 0, p = dbuf + (y + dy) * dpitch + 3); x < dx; x++, p += 4)
                *p = clrval;

        p = dbuf + (y + dy) * dpitch + 3 + (dx * 4);
        q = sbuf + (y + sy) * spitch + 3 + (sx * 4);
        for (x = 0; x < w; x++, p += 4, q += 4)
        {
            unsigned long n = (unsigned long)(*p) * (unsigned long)(*q);
            *p = (unsigned char)((n + (n >> 7)) >> 8);
        }
        if (clr)
            for (x = dx + w; x < diw; x++, p += 4)
                *p = clrval;
    }
    return TJS_S_OK;
none:
    // 領域範囲外で演算が行われない場合
    if (clr)
    {
        if(OperateGPUMask(dst,"FillMask",tTVPRect(0,0,diw,dih),nullptr,tTVPRect(),clrval)) return TJS_S_OK;
        destinationAccess.Acquire(dst,true,"layerExBTOA.clipAlphaRect");
        dbuf=static_cast<WrtRefT>(destinationAccess.Data()); dpitch=destinationAccess.Pitch();
        if(!dbuf) TVPThrowExceptionMessage(TJS_N("Layer has no images."));
        destinationAccess.Written(tTVPRect(0,0,diw,dih));
        for (long y = 0; y < dih; y++)
        {
            WrtRefT p = dbuf + y * dpitch + 3;
            for (long x = 0; x < diw; x++, p += 4)
                *p = clrval;
        }
    }
    return TJS_S_OK;
}

#define DWORD unsigned short
static tjs_error fillByProvince(tTJSVariant* result,
                                tjs_int numparams,
                                tTJSVariant** param,
                                iTJSDispatch2* lay)
{
    krkrsdl3::layer_work::SourceScope origin("layerExBTOA.fillByProvince");
    iTJSDispatch2* layerClass = getLayerClass();

    if (numparams < 2)
        return TJS_E_BADPARAMCOUNT;
    unsigned char index = (int)*param[0];
    DWORD color = (int)*param[1];

    // 書き込み先
    tTVPScopedLayerPixels access;
    WrtRefT dbuf = 0;
    long dw, dh, dpitch;
    if (!GetLayerBufferAndSize(lay, dw, dh, dbuf, dpitch, access))
    {
        TVPThrowExceptionMessage(TJS_N("must be Layer."));
    }

    ReadRefT sbuf = 0;
    long spitch;
    {
        tTJSVariant val;
        if (TJS_FAILED(layerClass->PropGet(0, TJS_N("provinceImageBuffer"), 0, &val, lay)) ||
            (sbuf = reinterpret_cast<ReadRefT>(val.AsInteger())) == NULL)
        {
            TVPThrowExceptionMessage(TJS_N("no province image."));
        }
        if (TJS_FAILED(layerClass->PropGet(0, TJS_N("provinceImageBufferPitch"), 0, &val, lay)) ||
            (spitch = (long)val.AsInteger()) == 0)
        {
            TVPThrowExceptionMessage(TJS_N("no province pitch."));
        }
    }

    for (int y = 0; y < dh; y++)
    {
        ReadRefT q = sbuf;
        DWORD* p = (DWORD*)dbuf;
        ttstr s;
        for (int x = 0; x < dw; x++)
        {
            if (*q == index)
            {
                *(DWORD*)p = color;
            }
            q++;
            p++;
        }
        sbuf += spitch;
        dbuf += dpitch;
    }
    return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(copyRightBlueToLeftAlpha, Layer, copyRightBlueToLeftAlpha);
NCB_ATTACH_FUNCTION(copyBottomBlueToTopAlpha, Layer, copyBottomBlueToTopAlpha);
NCB_ATTACH_FUNCTION(fillAlpha, Layer, fillAlpha);

NCB_ATTACH_FUNCTION(copyAlphaToProvince, Layer, copyAlphaToProvince);
NCB_ATTACH_FUNCTION(clipAlphaRect, Layer, clipAlphaRect);
NCB_ATTACH_FUNCTION(fillByProvince, Layer, fillByProvince);
