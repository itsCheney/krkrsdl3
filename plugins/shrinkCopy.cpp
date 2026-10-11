#include "ncbind/ncbind.hpp"
#include "ScopedLayerPixels.h"
#include "LayerShrinkGeometry.h"
#include "CPUConsumerTrace.h"
#include "PointReadTrace.h"
#include "LayerBitmap.h"
#include <cmath>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>

// A native operation retains the pre-COW/pre-resize source without creating
// image sharing. This lifetime reference is not a CPU pixel lease.
struct ShrinkSourceRef {
    iTVPTexture2D* texture=nullptr;
    ~ShrinkSourceRef() { if(texture) texture->ReleaseCPUAccessRef(); }
    void Capture(iTVPTexture2D* value) { texture=value; if(value) value->AddCPUAccessRef(); }
};
static bool ShrinkRealInRange(double value) {
    return std::isfinite(value) && value>double(std::numeric_limits<long>::min()) &&
           value<=double(std::numeric_limits<long>::max()) &&
           value<std::ldexp(1.0,int(sizeof(long)*8-1));
}
static bool ShrinkIntegerInRange(tjs_int64 value) {
    return value>=std::numeric_limits<long>::min() && value<=std::numeric_limits<long>::max();
}
static bool ShrinkProfileActive() {
    const uint64_t epoch=krkrsdl3::layer_work::CaptureGeneration();
    return epoch && krkrsdl3::layer_work::shrinkScope &&
           krkrsdl3::layer_work::shrinkScope->MatchesEpoch(epoch);
}
static const char* ShrinkReason(TVPLayerShrinkResult result) {
    switch(result) {
    case TVPLayerShrinkResult::Applied: return "applied";
    case TVPLayerShrinkResult::Unsupported: return "unsupported";
    case TVPLayerShrinkResult::Geometry: return "geometry";
    case TVPLayerShrinkResult::Resource: return "resource";
    case TVPLayerShrinkResult::CPUAccess: return "cpuAccess";
    case TVPLayerShrinkResult::AliasDependency: return "aliasDependency";
    case TVPLayerShrinkResult::Arithmetic: return "arithmetic";
    case TVPLayerShrinkResult::ParameterBudget: return "parameterBudget";
    case TVPLayerShrinkResult::BackendFailure: return "backendFailure";
    }
    return "unsupported";
}

#define NCB_MODULE_NAME TJS_N("shrinkCopy.dll")

struct LayerUtils
{
    // １ピクセルの型
    typedef uint32_t PixelT;

    // バッファ参照用の型
    typedef unsigned char UnitT;
    typedef UnitT const* BufRefT;
    typedef UnitT* WrtRefT;
    typedef tTVReal real;

    static bool IsValidLayer(iTJSDispatch2* lay)
    {
        // レイヤインスタンス以外ではエラー
        if (!lay || TJS_FAILED(lay->IsInstanceOf(0, 0, 0, TJS_N("Layer"), lay)))
            return false;

        // レイヤイメージは在るか？
        tTJSVariant val;
        if (TJS_FAILED(lay->PropGet(0, TJS_N("hasImage"), 0, &val, lay)) || (val.AsInteger() == 0))
            return false;

        return true;
    }

    /**
     * レイヤのサイズとバッファを取得する
     */
    static bool GetLayerSize(iTJSDispatch2* lay, long& w, long& h, long& pitch)
    {
        if (!IsValidLayer(lay))
            return false;
        tTJSNI_BaseLayer* native=nullptr;
        if(TJS_FAILED(lay->NativeInstanceSupport(TJS_NIS_GETINSTANCE,tTJSNC_Layer::ClassID,
            reinterpret_cast<iTJSNativeInstance**>(&native))) || !native || !native->GetMainImage() ||
            native->GetMainImage()->GetTexture()->GetFormat()!=TVPTextureFormat::RGBA) return false;

        // レイヤサイズを取得
        tTJSVariant val;
        if (TJS_FAILED(lay->PropGet(0, TJS_N("imageWidth"), 0, &val, lay)))
            return false;
        if(!ShrinkIntegerInRange(val.AsInteger())) return false;
        w = (long)val.AsInteger();

        val.Clear();
        if (TJS_FAILED(lay->PropGet(0, TJS_N("imageHeight"), 0, &val, lay)))
            return false;
        if(!ShrinkIntegerInRange(val.AsInteger())) return false;
        h = (long)val.AsInteger();

        // ピッチ取得
        val.Clear();
        if (TJS_FAILED(lay->PropGet(0, TJS_N("mainImageBufferPitch"), 0, &val, lay)))
            return false;
        if(!ShrinkIntegerInRange(val.AsInteger())) return false;
        pitch = (long)val.AsInteger();

        // 正常な値かどうか
        return (w > 0 && h > 0 && w<=INT32_MAX/4 && h<=INT32_MAX &&
                pitch>=w*4 && uint64_t(pitch)*uint64_t(h)<=uint64_t(PTRDIFF_MAX)/2);
    }

    // 読み込み用
    static bool GetLayerBufferAndSize(
        iTJSDispatch2* lay, long& w, long& h, BufRefT& ptr, long& pitch, tTVPScopedLayerPixels& access)
    {
        if (!GetLayerSize(lay, w, h, pitch))
            return false;

        access.Acquire(lay,false,"shrinkCopy.read");
        ptr=static_cast<BufRefT>(access.Data()); pitch=access.Pitch();
        return ptr!=nullptr && uint64_t(w)<=access.Width() && uint64_t(h)<=access.Height() &&
               pitch>=w*4 && uint64_t(pitch)*h<=uint64_t(PTRDIFF_MAX)/2;
    }

    // 書き込み用
    static bool GetLayerBufferAndSize(
        iTJSDispatch2* lay, long& w, long& h, WrtRefT& ptr, long& pitch, tTVPScopedLayerPixels& access)
    {
        if (!GetLayerSize(lay, w, h, pitch))
            return false;

        access.Acquire(lay,true,"shrinkCopy.write");
        ptr=static_cast<WrtRefT>(access.Data()); pitch=access.Pitch();
        return ptr!=nullptr && uint64_t(w)<=access.Width() && uint64_t(h)<=access.Height() &&
               pitch>=w*4 && uint64_t(pitch)*h<=uint64_t(PTRDIFF_MAX)/2;
    }
};

struct ShrinkCopy : public LayerUtils
{
    tTVPScopedLayerPixels sourceAccess, destinationAccess;
    ShrinkSourceRef retainedSource;
    tTJSNI_BaseLayer* nativeSource=nullptr;
    tTJSNI_BaseLayer* nativeDestination=nullptr;
    bool canonical=false;
    // TJS Method
    static tjs_error(layerShrinkCopy)(tTJSVariant* result,
                                      tjs_int numparams,
                                      tTJSVariant** param,
                                      iTJSDispatch2* dst)
    {
        if (numparams < 9)
            return TJS_E_BADPARAMCOUNT;
        krkrsdl3::cpu_consumer_trace::ConsumerScope consumer("Layer.shrinkCopy",
            krkrsdl3::cpu_consumer_trace::Access::Write,"native.shrinkCopy",reinterpret_cast<uintptr_t>(dst));
        krkrsdl3::point_trace::WriterScope writer("Layer.shrinkCopy");
        krkrsdl3::layer_work::ShrinkScope profile("shrinkCopy");
        for(int i=5;i<9;++i) if(!ShrinkIntegerInRange(param[i]->AsInteger())) return TJS_E_INVALIDPARAM;
        ShrinkCopy inst(dst, param[0]->AsReal(), param[1]->AsReal(), param[2]->AsReal(),
                        param[3]->AsReal(), param[4]->AsObjectNoAddRef(),
                        (long)param[5]->AsInteger(), (long)param[6]->AsInteger(),
                        (long)param[7]->AsInteger(), (long)param[8]->AsInteger());
        if (!inst.check())
        {
            krkrsdl3::layer_work::RecordShrinkResult(false,"invalidParameter",0);
            return TJS_E_INVALIDPARAM;
        }
        if (!inst.clip())
        {
            krkrsdl3::layer_work::RecordShrinkResult(false,"noop",0);
            return TJS_S_OK;
        }
        if(!inst.copy()) {
            krkrsdl3::layer_work::RecordShrinkResult(false,"invalidParameter",0);
            return TJS_E_INVALIDPARAM;
        }
        krkrsdl3::cpu_consumer_trace::MarkShrinkSuccess();
        return TJS_S_OK;
    }

    ShrinkCopy(iTJSDispatch2* _dst,
               real _dx,
               real _dy,
               real _dw,
               real _dh,
               iTJSDispatch2* _src,
               long _sx,
               long _sy,
               long _sw,
               long _sh)
      : dst(_dst),
        dx(_dx),
        dy(_dy),
        dw(_dw),
        dh(_dh),
        src(_src),
        sx(_sx),
        sy(_sy),
        sw(_sw),
        sh(_sh),
        ps(0),
        siw(0),
        sih(0),
        spch(0),
        pd(0),
        diw(0),
        dih(0),
        dpch(0)
    {
    }

    bool check()
    {
        if (!ShrinkRealInRange(dx) || !ShrinkRealInRange(dy) || !ShrinkRealInRange(dw) ||
            !ShrinkRealInRange(dh) ||
            !ShrinkRealInRange(dx+dw) || !ShrinkRealInRange(dy+dh) ||
            sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || sw < (long)dw || sh < (long)dh ||
            sw>INT32_MAX || sh>INT32_MAX || sx<INT32_MIN || sx>INT32_MAX ||
            sy<INT32_MIN || sy>INT32_MAX || int64_t(sx)+sw>LONG_MAX || int64_t(sy)+sh>LONG_MAX)
            return false;

        canonical=TVPGetCanonicalShrinkLayer(src,false,nativeSource) &&
                  TVPGetCanonicalShrinkLayer(dst,false,nativeDestination);
        if(canonical) {
            auto* sourceImage=nativeSource->GetMainImage();
            auto* destinationImage=nativeDestination->GetMainImage();
            if(!sourceImage || !destinationImage) return false;
            auto* texture=sourceImage->GetTexture();
            auto* target=destinationImage->GetTexture();
            if(!texture || !target || texture->GetFormat()!=TVPTextureFormat::RGBA ||
               target->GetFormat()!=TVPTextureFormat::RGBA) return false;
            siw=texture->GetWidth(); sih=texture->GetHeight(); spch=texture->GetPitch();
            diw=target->GetWidth(); dih=target->GetHeight(); dpch=target->GetPitch();
            if(siw<=0 || sih<=0 || diw<=0 || dih<=0 || siw>INT32_MAX/4 || diw>INT32_MAX/4 ||
               spch<siw*4 || dpch<diw*4 || uint64_t(spch)*sih>uint64_t(PTRDIFF_MAX)/2 ||
               uint64_t(dpch)*dih>uint64_t(PTRDIFF_MAX)/2) return false;
            retainedSource.Capture(texture);
            krkrsdl3::layer_work::SetShrinkAliasClass(texture==target ? "trueAlias" :
                (!texture->IsIndependent() ? "shared" : "distinct"));
            return true;
        }
        krkrsdl3::layer_work::SetShrinkAliasClass("customBinding");

        // サイズ取得
        if (!GetLayerBufferAndSize(src, siw, sih, ps, spch, sourceAccess) ||
            !GetLayerBufferAndSize(dst, diw, dih, pd, dpch, destinationAccess))
            return false;

        return true;
    }

    static inline long RtoL(real const r) { return ((r) < 0) ? -(long)(-(r)) : (long)(r); }

    bool clip()
    {
        real zx = dw / (real)sw;
        real zy = dh / (real)sh;
        real dcut;
        long scut;

        // srcクリッピング
        if (sx + sw <= 0 || sy + sh <= 0 || sx >= siw || sy >= sih)
            return false;

        if (sx < 0)
        {
            sw += sx;
            dw -= (dcut = zx * (real)(-sx));
            sx = 0;
            dx += dcut;
        }
        if (sy < 0)
        {
            sh += sy;
            dh -= (dcut = zy * (real)(-sy));
            sy = 0;
            dy += dcut;
        }
        if ((scut = sx + sw - siw) > 0)
            (sw -= scut), (dw -= zx * (real)(scut));
        if ((scut = sy + sh - sih) > 0)
            (sh -= scut), (dh -= zy * (real)(scut));

        // dstの整数位置
        dtx = RtoL(dx);
        dty = RtoL(dy);
        dtw = RtoL(dx + dw) - dtx;
        dth = RtoL(dy + dh) - dty;
        if ((dx + dw) > (real)(dtx + dtw))
            dtw++;
        if ((dy + dh) > (real)(dty + dth))
            dth++;

        // dstクリッピング
        if (dtx + dtw <= 0 || dty + dth <= 0 || dtx >= diw || dty >= dih)
            return false;

        // dstクリッピング範囲
        dsx = (dtx < 0) ? -dtx : 0;
        dsy = (dty < 0) ? -dty : 0;
        dex = (dtx + dtw > diw) ? (diw - dtx) : dtw;
        dey = (dty + dth > dih) ? (dih - dty) : dth;

        return true;
    }

    typedef unsigned long AvgT;
    typedef unsigned char uchar;
    struct AvgInfoT
    {
        ptrdiff_t offset;
        int step;
        AvgT ta, tc, ba, bc, total;
    };
    struct ElementT
    {
        AvgT r, g, b, a;
    };

    bool copy()
    {
        const uint64_t prepStarted=ShrinkProfileActive() ? krkrsdl3::layer_work::Now() : 0;
        AvgInfoT *horz = 0, *vert = 0;
        void* buf = allocAvgBuffer(horz, vert, dex - dsx, dey - dsy);
        if (!horz || !vert)
            throw std::bad_alloc();
        std::unique_ptr<void,decltype(&std::free)> buffer(buf,&std::free);

        // 縦横別に平均化テーブル作成
        AvgT hu,vu;
        try { hu=makeAvgTable(horz,true); vu=makeAvgTable(vert,false); }
        catch(const std::range_error&) { return false; }
        AvgT unit = hu * vu;

        TVPLayerShrinkOperation operation;
        operation.kind=TVPLayerShrinkKind::Area;
        operation.avgBits=sizeof(AvgT)*8;
        operation.destination={int(dtx+dsx),int(dty+dsy),int(dtx+dex),int(dty+dey)};
        operation.hu=uint32_t(hu); operation.vu=uint32_t(vu);
        const auto axis=[](const AvgInfoT* table,long count,long stride) {
            auto list=std::make_shared<std::vector<TVPLayerShrinkAxis>>();
            list->reserve(size_t(count));
            for(long i=0;i<count;++i) list->push_back({int32_t(table[i].offset/stride),table[i].step,
                table[i].ta,table[i].tc,table[i].ba,table[i].bc,table[i].total});
            return list;
        };
        operation.horizontal=axis(horz,dex-dsx,4);
        operation.vertical=axis(vert,dey-dsy,spch);
        TVPLayerShrinkGeometry::Validation validation;
        auto status=TVPLayerShrinkGeometry::Validate(operation,int(siw),int(sih),int(diw),int(dih),false,validation);
        // A budget or alias rejection still has a safe, exact CPU implementation.
        if(status==TVPLayerShrinkResult::Arithmetic || status==TVPLayerShrinkResult::Geometry) return false;
        operation.sourceTop=validation.sourceTop; operation.sourceRows=validation.sourceRows;
        if(prepStarted && ShrinkProfileActive()) {
            char metadata[256];
            std::snprintf(metadata,sizeof(metadata),"{\"source\":[%ld,%ld],\"roi\":[%d,%d,%d,%d],\"avgBits\":%u}",
                siw,sih,operation.destination.left,operation.destination.top,operation.destination.right,
                operation.destination.bottom,operation.avgBits);
            krkrsdl3::layer_work::SetShrinkMetadata(metadata);
            krkrsdl3::layer_work::RecordShrinkPrepCPU(krkrsdl3::layer_work::Now()-prepStarted);
        }
        if(canonical) {
            if(status==TVPLayerShrinkResult::Applied && TVPHasMetalLayerShrinkSupport()) {
                auto* target=nativeDestination->GetMainImageTextureForCPUAccess(true);
                if(ShrinkProfileActive()) {
                    if(target==retainedSource.texture)
                        krkrsdl3::layer_work::SetShrinkAliasClass(TVPLayerShrinkGeometry::AliasSafe(operation) ? "safeAlias" : "unsafeAlias");
                    else if(src==dst) krkrsdl3::layer_work::SetShrinkAliasClass("cowSeparated");
                }
                status=TVPTryMetalLayerShrink(operation,target,retainedSource.texture);
                if(status==TVPLayerShrinkResult::Applied) {
                    krkrsdl3::layer_work::RecordShrinkResult(true,"applied",uint64_t(dex-dsx)*(dey-dsy));
                    return true;
                }
            } else if(status==TVPLayerShrinkResult::Applied) status=TVPLayerShrinkResult::Unsupported;
            sourceAccess.tTVPScopedTexturePixels::Acquire(retainedSource.texture,false,"shrinkCopy.read");
            ps=static_cast<BufRefT>(sourceAccess.Data()); spch=sourceAccess.Pitch();
            destinationAccess.Acquire(dst,true,"shrinkCopy.write");
            pd=static_cast<WrtRefT>(destinationAccess.Data()); dpch=destinationAccess.Pitch();
            if(!ps || !pd) return false;
        }
        krkrsdl3::layer_work::RecordShrinkResult(false,canonical ? ShrinkReason(status) : "customBinding",
                                               uint64_t(dex-dsx)*(dey-dsy));
        destinationAccess.Written(tTVPRect(dtx+dsx,dty+dsy,dtx+dex,dty+dey));

        WrtRefT p, pl = pd + ptrdiff_t(dtx + dsx) * 4 + ptrdiff_t(dty + dsy) * dpch;
        AvgInfoT const* vi = vert;
        for (long x, y = dsy; y < dey; y++, vi++)
        {
            AvgInfoT const* hi = horz;
            for (p = pl, x = dsx; x < dex; x++, hi++, p += 4)
            {
                ElementT sum = {0, 0, 0, 0};
                const ptrdiff_t offset=vi->offset + hi->offset;
                const int w = hi->step;
                const int h = vi->step;
                const ptrdiff_t ox = ptrdiff_t(w) * 4;
                const ptrdiff_t oy = ptrdiff_t(h) * spch;
                // Form a pointer only for a sample that is actually read. The
                // base can sit beyond an edge when step=-1 or a gate is zero.
                if(hi->ta && vi->ta) addPoint(sum, ps + (offset - 4 - spch), hi->ta * vi->ta, hi->tc * vi->tc);
                if(w>0 && vi->ta) addHorz(sum, ps + (offset - spch), w, hu * vi->ta, hu * vi->tc);
                if(hi->ba && vi->ta) addPoint(sum, ps + (offset + ox - spch), hi->ba * vi->ta, hi->bc * vi->tc);
                if(h>0 && hi->ta) addVert(sum, ps + (offset - 4), h, hi->ta * vu, hi->tc * vu);
                if(w>0 && h>0) addRect(sum, ps + offset, w, h, unit);
                if(h>0 && hi->ba) addVert(sum, ps + (offset + ox), h, hi->ba * vu, hi->bc * vu);
                if(hi->ta && vi->ba) addPoint(sum, ps + (offset - 4 + oy), hi->ta * vi->ba, hi->tc * vi->bc);
                if(w>0 && vi->ba) addHorz(sum, ps + (offset + oy), w, hu * vi->ba, hu * vi->bc);
                if(hi->ba && vi->ba) addPoint(sum, ps + (offset + ox + oy), hi->ba * vi->ba, hi->bc * vi->bc);
                AvgT div = hi->total * vi->total;
                p[0] = (uchar)(sum.r / div);
                p[1] = (uchar)(sum.g / div);
                p[2] = (uchar)(sum.b / div);
                p[3] = (uchar)(sum.a / div);
            }
            if(y+1<dey) pl+=dpch;
        }
        return true;
    }

    struct MakeAvgWorkT
    {
        AvgT unit;
        real max, ratio, diff;
        long stop, ofmul;
    };
    static ptrdiff_t tableOffset(int64_t base,long stride) {
        const int64_t offset=base*int64_t(stride);
        if(offset< std::numeric_limits<ptrdiff_t>::min()/2 ||
           offset> std::numeric_limits<ptrdiff_t>::max()/2)
            throw std::range_error("shrink table offset");
        return ptrdiff_t(offset);
    }
    AvgT makeAvgTable(AvgInfoT* tbl, bool isHorz)
    {
        MakeAvgWorkT work;
        long pos, end;
        if (isHorz)
        {
            pos = dsx;
            end = dex - 1;
            work.max = (real)sw;
            work.ratio = work.max / dw;
            work.diff = dx - (real)dtx;
            work.stop = sx;
            work.ofmul = 4;
        }
        else
        {
            pos = dsy;
            end = dey - 1;
            work.max = (real)sh;
            work.ratio = work.max / dh;
            work.diff = dy - (real)dty;
            work.stop = sy;
            work.ofmul = spch;
        }
        work.unit = 256; // 1dotの解像度
        if (work.ratio <= 1.0 / 16)
        {
            // 1/16以下で桁あふれの可能性があるのでunitを小さくする
            work.unit /= (int)((2.0 / 16.0) / work.ratio);
            if (work.unit <= 0)
                work.unit = 1;
        }
        if (pos == end)
        {
            // 縮小幅が１ドットの場合
            setAvgInfoEdge(work, tbl, pos);
        }
        else
        {
            setAvgInfoEdge(work, tbl++, pos++);
            while (pos < end)
                setAvgInfo(work, tbl++, pos++);
            setAvgInfoEdge(work, tbl++, pos++);
        }
        return work.unit;
    }
    // AvgInfo設定(端以外:ta==tc,ba==bc)
    inline void setAvgInfo(MakeAvgWorkT const& wk, AvgInfoT* tbl, long pos)
    {
        AvgT const unit = wk.unit;
        real r1, r2;
        r1 = ((real)pos - wk.diff) * (r2 = wk.ratio);
        r2 += r1;
        if(!ShrinkRealInRange(r1) || !ShrinkRealInRange(r2)) throw std::range_error("shrink table coordinate");
        long t1 = RtoL(r1), t2 = RtoL(r2);
        if(int64_t(wk.stop)+t1+1<INT32_MIN || int64_t(wk.stop)+t1+1>INT32_MAX ||
           int64_t(t2)-t1-1< -1 || int64_t(t2)-t1-1>INT32_MAX) throw std::range_error("shrink table span");
        tbl->offset = tableOffset(int64_t(wk.stop) + t1 + 1,wk.ofmul);
        tbl->total = ((tbl->ta = tbl->tc = (unit - RtoL((r1 - (real)t1) * unit))) +
                      (tbl->ba = tbl->bc = (RtoL((r2 - (real)t2) * unit))) +
                      (tbl->step = int(int64_t(t2) - t1 - 1)) * unit);
    }
    // AvgInfo設定(端例外:ta!=tc,ba!=bc)
    inline void setAvgInfoEdge(MakeAvgWorkT const& wk, AvgInfoT* tbl, long pos)
    {
        AvgT const unit = wk.unit;
        real r1, r2, f1, f2;
        r1 = ((real)pos - wk.diff) * (r2 = wk.ratio);
        r2 += r1;
        if(!ShrinkRealInRange(r1) || !ShrinkRealInRange(r2)) throw std::range_error("shrink table coordinate");
        if ((f1 = r1) < 0)
            f1 = 0;
        if ((f2 = r2) > wk.max)
            f2 = wk.max;
        long t1 = RtoL(r1), t2 = RtoL(r2);
        long u1 = RtoL(f1), u2 = RtoL(f2);
        if(int64_t(wk.stop)+u1+1<INT32_MIN || int64_t(wk.stop)+u1+1>INT32_MAX ||
           int64_t(u2)-u1-1< -1 || int64_t(u2)-u1-1>INT32_MAX) throw std::range_error("shrink table span");
        tbl->offset = tableOffset(int64_t(wk.stop) + u1 + 1,wk.ofmul);
        tbl->total = ((tbl->tc = (unit - RtoL((r1 - (real)t1) * unit))) +
                      (tbl->bc = (RtoL((r2 - (real)t2) * unit))) + AvgT(int64_t(t2) - t1 - 1) * unit);
        /**/ tbl->ta = (unit - RtoL((f1 - (real)u1) * unit));
        /**/ tbl->ba = (RtoL((f2 - (real)u2) * unit));
        /**/ tbl->step = int(int64_t(u2) - u1 - 1);
    }

    void* allocAvgBuffer(AvgInfoT*& hi, AvgInfoT*& vi, long w, long h)
    {
        if(w<=0 || h<=0 || uint64_t(w)+uint64_t(h)>SIZE_MAX/sizeof(AvgInfoT)) return nullptr;
        void* ret = malloc(sizeof(AvgInfoT) * (size_t(w) + size_t(h)));
        hi = (AvgInfoT*)ret;
        vi = hi ? hi + w : nullptr;
        return ret;
    }
    void freeAvgBuffer(void* buf)
    {
        if (buf)
            free(buf);
    }

    inline void addRect(ElementT& sum, BufRefT p, int w, int h, AvgT mul)
    {
        if (mul)
            for (; h > 0; h--)
            {
                PixelT* r = (PixelT*)p;
                for (int n = w; n > 0; n--, r++)
                {
                    PixelT col = *r;
                    sum.r += (col & 0xFF) * mul;
                    sum.g += ((col >> 8) & 0xFF) * mul;
                    sum.b += ((col >> 16) & 0xFF) * mul;
                    sum.a += ((col >> 24)) * mul;
                }
                if(h>1) p+=spch;
            }
    }
    inline void addVert(ElementT& sum, BufRefT p, int h, AvgT mul1, AvgT mul2)
    {
        if (mul1)
            for (; h > 0; h--)
            {
                PixelT col = *(PixelT*)p;
                sum.r += (col & 0xFF) * mul2;
                sum.g += ((col >> 8) & 0xFF) * mul2;
                sum.b += ((col >> 16) & 0xFF) * mul2;
                sum.a += ((col >> 24)) * mul1;
                if(h>1) p+=spch;
            }
    }
    inline void addHorz(ElementT& sum, BufRefT p, int w, AvgT mul1, AvgT mul2)
    {
        if (mul1)
            for (PixelT* r = (PixelT*)p; w > 0; w--, r++)
            {
                PixelT col = *r;
                sum.r += (col & 0xFF) * mul2;
                sum.g += ((col >> 8) & 0xFF) * mul2;
                sum.b += ((col >> 16) & 0xFF) * mul2;
                sum.a += ((col >> 24)) * mul1;
            }
    }
    inline void addPoint(ElementT& sum, BufRefT p, AvgT mul1, AvgT mul2)
    {
        if (mul1)
        {
            PixelT col = *(PixelT*)p;
            sum.r += (col & 0xFF) * mul2;
            sum.g += ((col >> 8) & 0xFF) * mul2;
            sum.b += ((col >> 16) & 0xFF) * mul2;
            sum.a += ((col >> 24)) * mul1;
        }
    }

protected:
    iTJSDispatch2* dst;
    real dx, dy, dw, dh;
    long dtx, dty, dtw, dth;
    long dsx, dsy, dex, dey;

    iTJSDispatch2* src;
    long sx, sy, sw, sh;

    BufRefT ps;
    long siw, sih, spch;

    WrtRefT pd;
    long diw, dih, dpch;
};
NCB_ATTACH_FUNCTION(shrinkCopy, Layer, ShrinkCopy::layerShrinkCopy);

struct LimitedShrink : public LayerUtils
{
    tTVPScopedLayerPixels sourceAccess, destinationAccess;
    ShrinkSourceRef retainedSource;
    tTJSNI_BaseLayer* nativeSource=nullptr;
    tTJSNI_BaseLayer* nativeDestination=nullptr;
    bool canonical=false;
    // TJS Method
    static tjs_error(layerShrinkCopy)(tTJSVariant* result,
                                      tjs_int numparams,
                                      tTJSVariant** param,
                                      iTJSDispatch2* dst)
    {
        if (numparams < 2)
            return TJS_E_BADPARAMCOUNT;
        krkrsdl3::cpu_consumer_trace::ConsumerScope consumer("Layer.shrinkCopyFast",
            krkrsdl3::cpu_consumer_trace::Access::Write,"native.shrinkCopy",reinterpret_cast<uintptr_t>(dst));
        krkrsdl3::point_trace::WriterScope writer("Layer.shrinkCopyFast");
        krkrsdl3::layer_work::ShrinkScope profile("shrinkCopyFast");
        if(!ShrinkIntegerInRange(param[1]->AsInteger()) ||
           (numparams>=3 && !ShrinkIntegerInRange(param[2]->AsInteger()))) return TJS_E_INVALIDPARAM;
        LimitedShrink inst(dst, param[0]->AsObjectNoAddRef(), (long)param[1]->AsInteger(),
                           (numparams >= 3) ? (long)param[2]->AsInteger() : 0);
        if (!inst.check())
        {
            krkrsdl3::layer_work::RecordShrinkResult(false,"invalidParameter",0);
            return TJS_E_INVALIDPARAM;
        }
        if (!inst.resize())
        {
            krkrsdl3::layer_work::RecordShrinkResult(false,"resizeFailure",0);
            return TJS_E_FAIL;
        }
        if(!inst.copy()) {
            krkrsdl3::layer_work::RecordShrinkResult(false,"invalidParameter",0);
            return TJS_E_INVALIDPARAM;
        }
        krkrsdl3::cpu_consumer_trace::MarkShrinkSuccess();
        return TJS_S_OK;
    }

    LimitedShrink(iTJSDispatch2* _dst, iTJSDispatch2* _src, long _stepx, long _stepy)
      : dst(_dst),
        src(_src),
        stepx(_stepx),
        stepy(_stepy),
        xdiv(0),
        xrem(false)
    {
        if (!stepy)
            stepy = stepx;
    }

    bool check()
    {
        if(stepx<=0 || stepy<=0 || stepx>INT32_MAX || stepy>INT32_MAX) return false;
        canonical=TVPGetCanonicalShrinkLayer(src,false,nativeSource) &&
                  TVPGetCanonicalShrinkLayer(dst,true,nativeDestination);
        if(canonical) {
            auto* image=nativeSource->GetMainImage();
            if(!image || !nativeDestination->GetMainImage()) return false;
            auto* texture=image->GetTexture();
            if(!texture || texture->GetFormat()!=TVPTextureFormat::RGBA ||
               nativeDestination->GetMainImage()->GetTexture()->GetFormat()!=TVPTextureFormat::RGBA) return false;
            siw=texture->GetWidth(); sih=texture->GetHeight(); spch=texture->GetPitch();
            if(siw<=0 || sih<=0 || siw>INT32_MAX/4 || spch<siw*4 ||
               uint64_t(spch)*sih>uint64_t(PTRDIFF_MAX)/2) return false;
            retainedSource.Capture(texture);
            krkrsdl3::layer_work::SetShrinkAliasClass(texture==nativeDestination->GetMainImage()->GetTexture() ?
                "trueAlias" : (!texture->IsIndependent() ? "shared" : "distinct"));
            return true;
        }
        krkrsdl3::layer_work::SetShrinkAliasClass("customBinding");
        return (stepx > 0 && stepy > 0 && GetLayerBufferAndSize(src, siw, sih, ps, spch, sourceAccess) &&
                IsValidLayer(dst));
    }
    bool resize()
    {
        tTJSVariant nw((tjs_int)(1+(siw-1)/stepx));
        tTJSVariant nh((tjs_int)(1+(sih-1)/stepy));
        tTJSVariant* param[] = {&nw, &nh};
        if(TJS_FAILED(dst->FuncCall(0,TJS_N("setImageSize"),0,nullptr,2,param,dst))) return false;
        if(canonical) {
            auto* target=nativeDestination->GetMainImage()->GetTexture();
            diw=target->GetWidth(); dih=target->GetHeight(); dpch=target->GetPitch();
            return diw==1+(siw-1)/stepx && dih==1+(sih-1)/stepy && dpch>=diw*4 &&
                   uint64_t(dpch)*dih<=uint64_t(PTRDIFF_MAX)/2;
        }
        return GetLayerBufferAndSize(dst,diw,dih,pd,dpch,destinationAccess) &&
               diw==1+(siw-1)/stepx && dih==1+(sih-1)/stepy;
    }
    bool copy()
    {
        const uint64_t prepStarted=ShrinkProfileActive() ? krkrsdl3::layer_work::Now() : 0;
        TVPLayerShrinkOperation operation;
        operation.kind=TVPLayerShrinkKind::Fast; operation.avgBits=32;
        operation.destination={0,0,int(diw),int(dih)};
        operation.sourceTop=0; operation.sourceRows=int(sih);
        const auto axis=[](long size,long step) {
            auto list=std::make_shared<std::vector<TVPLayerShrinkAxis>>();
            list->reserve(size_t(1+(size-1)/step));
            for(long base=0;base<size;) {
                const long count=std::min(step,size-base);
                list->push_back({int32_t(base),int32_t(count),0,0,0,0,uint64_t(count)});
                base+=count;
            }
            return list;
        };
        operation.horizontal=axis(siw,stepx); operation.vertical=axis(sih,stepy);
        TVPLayerShrinkGeometry::Validation validation;
        auto status=TVPLayerShrinkGeometry::Validate(operation,int(siw),int(sih),int(diw),int(dih),false,validation);
        if(status==TVPLayerShrinkResult::Arithmetic || status==TVPLayerShrinkResult::Geometry) return false;
        if(prepStarted && ShrinkProfileActive()) {
            char metadata[256];
            std::snprintf(metadata,sizeof(metadata),"{\"source\":[%ld,%ld],\"roi\":[0,0,%ld,%ld],\"step\":[%ld,%ld]}",
                siw,sih,diw,dih,stepx,stepy);
            krkrsdl3::layer_work::SetShrinkMetadata(metadata);
            krkrsdl3::layer_work::RecordShrinkPrepCPU(krkrsdl3::layer_work::Now()-prepStarted);
        }
        if(canonical) {
            if(status==TVPLayerShrinkResult::Applied && TVPHasMetalLayerShrinkSupport()) {
                auto* target=nativeDestination->GetMainImageTextureForCPUAccess(true);
                if(ShrinkProfileActive()) {
                    if(target==retainedSource.texture)
                        krkrsdl3::layer_work::SetShrinkAliasClass(TVPLayerShrinkGeometry::AliasSafe(operation) ? "safeAlias" : "unsafeAlias");
                    else if(src==dst) krkrsdl3::layer_work::SetShrinkAliasClass("resizedSource");
                }
                status=TVPTryMetalLayerShrink(operation,target,retainedSource.texture);
                if(status==TVPLayerShrinkResult::Applied) {
                    krkrsdl3::layer_work::RecordShrinkResult(true,"applied",uint64_t(diw)*dih);
                    return true;
                }
            } else if(status==TVPLayerShrinkResult::Applied) status=TVPLayerShrinkResult::Unsupported;
            // The resize already happened exactly once; a rejected GPU attempt
            // continues from the retained original source, never the new image.
            sourceAccess.tTVPScopedTexturePixels::Acquire(retainedSource.texture,false,"shrinkCopy.read");
            ps=static_cast<BufRefT>(sourceAccess.Data()); spch=sourceAccess.Pitch();
            destinationAccess.Acquire(dst,true,"shrinkCopy.write");
            pd=static_cast<WrtRefT>(destinationAccess.Data()); dpch=destinationAccess.Pitch();
            if(!ps || !pd) return false;
        }
        krkrsdl3::layer_work::RecordShrinkResult(false,canonical ? ShrinkReason(status) : "customBinding",
                                               uint64_t(diw)*dih);
        destinationAccess.Written(tTVPRect(0,0,diw,dih));
        xdiv = siw / stepx;
        xrem = siw - xdiv * stepx;
        if (stepy <= 1)
        {
            for (long y = 0; y < sih; y++, pd += dpch, ps += spch)
                shrinkLineX(pd, ps);
        }
        else
        {
            long bpch = diw * 4;
            const size_t rows=size_t(std::min(stepy,sih));
            if(rows>SIZE_MAX/size_t(bpch) || rows>size_t(PTRDIFF_MAX)/size_t(bpch)) throw std::bad_alloc();
            WrtRefT buf = new UnitT[size_t(bpch) * rows];
            try
            {
                long div = sih / stepy;
                for (long len = div; len > 0; len--, pd += dpch)
                {
                    size_t ofs=0;
                    for (long sub = stepy; sub > 0; sub--, ofs += size_t(bpch), ps += spch)
                        shrinkLineX(buf + ofs, ps);
                    shrinkLineY(pd, buf, bpch, stepy);
                }
                div *= stepy;
                if (div < sih)
                {
                    long yrem = sih - div;
                    size_t ofs=0;
                    for (long sub = yrem; sub > 0; sub--, ofs += size_t(bpch), ps += spch)
                        shrinkLineX(buf + ofs, ps);
                    shrinkLineY(pd, buf, bpch, yrem);
                }
            }
            catch (...)
            {
                delete[] buf;
                throw;
            }
            delete[] buf;
        }
        return true;
    }
    static inline void ShrinkLine(
        WrtRefT& w, BufRefT& r, long len, long shrink, long step, long tstep)
    {
        BufRefT tr = r;
        switch (shrink)
        {
            case 1:
                for (; len > 0; len--, r += step)
                {
                    *w++ = r[0];
                    *w++ = r[1];
                    *w++ = r[2];
                    *w++ = 255;
                }
                break;
            case 2:
            case 3:
            case 4:
                /* 専用処理を書く */
            default:
                for (PixelT sr, sg, sb; len > 0; len--, r += step)
                {
                    sr = sg = sb = 0;
                    tr = r;
                    for (long sub = shrink; sub > 0; sub--)
                    {
                        sr += (PixelT)tr[0];
                        sg += (PixelT)tr[1];
                        sb += (PixelT)tr[2];
                        if(sub>1) tr+=tstep;
                    }
                    *w++ = (UnitT)(sr / shrink);
                    *w++ = (UnitT)(sg / shrink);
                    *w++ = (UnitT)(sb / shrink);
                    *w++ = 255;
                }
        }
    }

    void shrinkLineX(WrtRefT w, BufRefT r)
    {
        ShrinkLine(w, r, xdiv, stepx, std::min(stepx,siw) * 4, 4);
        if (xrem > 0)
            ShrinkLine(w, r, 1, xrem, 0, 4);
    }
    inline void shrinkLineY(WrtRefT w, BufRefT r, long pch, long shrink)
    {
        ShrinkLine(w, r, diw, shrink, 4, pch);
    }

protected:
    iTJSDispatch2* dst;
    iTJSDispatch2* src;
    long stepx, stepy, xdiv, xrem;

    BufRefT ps;
    long siw, sih, spch;

    WrtRefT pd;
    long diw, dih, dpch;
};
NCB_ATTACH_FUNCTION(shrinkCopyFast, Layer, LimitedShrink::layerShrinkCopy);
