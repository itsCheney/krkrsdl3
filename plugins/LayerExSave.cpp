// layerExSave.dll
//
// Port of wtnbgo's layerExSave (Window async TLG5/PNG save, Layer sync save
// and the Layer/Math utility functions). The encoders work on a tight BGRA
// snapshot taken on the main thread, so the worker thread never touches TJS
// objects, layer textures or storage. Encoded bytes are written to storage on
// the main thread when the done event is delivered.

#include "ncbind/ncbind.hpp"
#include "ScopedLayerPixels.h"
#include "SaveTLG.h"
#include "TVPEvent.h"
#include "TVPStorage.h"
#include "TVPDebug.h"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define NCB_MODULE_NAME TJS_N("layerExSave.dll")

namespace {

typedef std::vector<tjs_uint8> Bytes;
// Returns true when the save has been canceled.
typedef std::function<bool(int percent)> ProgressFunc;

//----------------------------------------------
// Layer pixel access

bool GetLayerSize(iTJSDispatch2* lay, long& w, long& h)
{
    if (!lay || TJS_FAILED(lay->IsInstanceOf(0, 0, 0, TJS_N("Layer"), lay)))
        return false;
    tTJSVariant val;
    if (TJS_FAILED(lay->PropGet(0, TJS_N("hasImage"), 0, &val, lay)) || val.AsInteger() == 0)
        return false;
    val.Clear();
    if (TJS_FAILED(lay->PropGet(0, TJS_N("imageWidth"), 0, &val, lay)))
        return false;
    w = (long)val.AsInteger();
    val.Clear();
    if (TJS_FAILED(lay->PropGet(0, TJS_N("imageHeight"), 0, &val, lay)))
        return false;
    h = (long)val.AsInteger();
    return w > 0 && h > 0;
}

// Main image pixels (BGRA, 4 bytes per pixel) held for the lifetime of the
// object. A write lease marks the whole image as written.
struct LayerPixels
{
    tTVPScopedLayerPixels access;
    tjs_uint8* data = nullptr;
    long w = 0, h = 0, pitch = 0;

    bool Open(iTJSDispatch2* lay, bool write, const char* caller)
    {
        if (!GetLayerSize(lay, w, h))
            return false;
        access.Acquire(lay, write, caller);
        data = static_cast<tjs_uint8*>(access.Data());
        if (!data)
            return false;
        pitch = access.Pitch();
        w = std::min<long>(w, (long)access.Width());
        h = std::min<long>(h, (long)access.Height());
        if (write)
            access.Written(tTVPRect(0, 0, w, h));
        return w > 0 && h > 0 && pitch != 0;
    }
    const tjs_uint8* Line(long y) const { return data + pitch * y; }
    tjs_uint8* Line(long y) { return data + pitch * y; }
};

void OpenOrThrow(LayerPixels& pixels, iTJSDispatch2* lay, bool write, const char* caller,
                 const tjs_char* message = TJS_N("Invalid layer image."))
{
    if (!pixels.Open(lay, write, caller))
        TVPThrowExceptionMessage(message);
}

// Tight BGRA copy of a layer image, safe to hand to a worker thread.
struct Image
{
    long w = 0, h = 0;
    Bytes px;
    const tjs_uint8* Line(long y) const { return px.data() + (size_t)w * 4 * y; }
};

bool Snapshot(iTJSDispatch2* lay, Image& image)
{
    LayerPixels pixels;
    if (!pixels.Open(lay, false, "layerExSave.snapshot"))
        return false;
    image.w = pixels.w;
    image.h = pixels.h;
    const size_t row = (size_t)image.w * 4;
    image.px.resize(row * image.h);
    for (long y = 0; y < image.h; y++)
        std::memcpy(image.px.data() + row * y, pixels.Line(y), row);
    return true;
}

//----------------------------------------------
// Byte writers

void PutBE32(Bytes& out, tjs_uint32 v)
{
    out.push_back((tjs_uint8)(v >> 24));
    out.push_back((tjs_uint8)(v >> 16));
    out.push_back((tjs_uint8)(v >> 8));
    out.push_back((tjs_uint8)v);
}

void PutLE32(Bytes& out, tjs_uint32 v)
{
    out.push_back((tjs_uint8)v);
    out.push_back((tjs_uint8)(v >> 8));
    out.push_back((tjs_uint8)(v >> 16));
    out.push_back((tjs_uint8)(v >> 24));
}

void SetLE32(Bytes& out, size_t pos, tjs_uint32 v)
{
    out[pos] = (tjs_uint8)v;
    out[pos + 1] = (tjs_uint8)(v >> 8);
    out[pos + 2] = (tjs_uint8)(v >> 16);
    out[pos + 3] = (tjs_uint8)(v >> 24);
}

void PutBytes(Bytes& out, const void* data, size_t size)
{
    const tjs_uint8* p = static_cast<const tjs_uint8*>(data);
    out.insert(out.end(), p, p + size);
}

// Reports each percentage once.
struct Progress
{
    const ProgressFunc& func;
    int last = -1;
    explicit Progress(const ProgressFunc& f) : func(f) {}
    bool operator()(int percent)
    {
        if (!func || percent == last)
            return false;
        last = percent;
        return func(percent);
    }
};

//----------------------------------------------
// TLG5

// Tag dictionary as the TLG0 "tags" chunk: "len:name=len:value," per member.
std::string BuildTlgTags(iTJSDispatch2* dict)
{
    std::string tags;
    if (!dict)
        return tags;
    struct Callback : public tTJSDispatch
    {
        std::string& Tags;
        explicit Callback(std::string& tags) : Tags(tags) {}
        tjs_error FuncCall(tjs_uint32, const tjs_char*, tjs_uint32*, tTJSVariant* result,
                           tjs_int numparams, tTJSVariant** param, iTJSDispatch2*) override
        {
            if (numparams < 3)
                return TJS_E_BADPARAMCOUNT;
            if (!((tjs_int)*param[1] & TJS_HIDDENMEMBER))
            {
                std::string name = ttstr(*param[0]).AsStdString();
                std::string value = ttstr(*param[2]).AsStdString();
                Tags += std::to_string(name.size()) + ":" + name + "=" +
                        std::to_string(value.size()) + ":" + value + ",";
            }
            if (result)
                *result = (tjs_int)1;
            return TJS_S_OK;
        }
    } callback(tags);
    tTJSVariantClosure closure(&callback, NULL);
    dict->EnumMembers(TJS_IGNOREPROP, &closure, dict);
    return tags;
}

// Returns true when canceled.
bool EncodeTlg5Raw(const Image& image, Bytes& out, Progress& progress)
{
    const int colors = 4;
    const long blockHeight = 4;
    const long width = image.w, height = image.h;

    PutBytes(out, "TLG5.0\x00raw\x1a\x00", 11);
    out.push_back((tjs_uint8)colors);
    PutLE32(out, (tjs_uint32)width);
    PutLE32(out, (tjs_uint32)height);
    PutLE32(out, (tjs_uint32)blockHeight);

    const long blockCount = (height - 1) / blockHeight + 1;
    const size_t blockSizePos = out.size();
    out.resize(out.size() + blockCount * 4);

    std::unique_ptr<SlideCompressor> compressor(new SlideCompressor());
    std::vector<Bytes> inbuf(colors, Bytes(width * blockHeight));
    std::vector<Bytes> outbuf(colors, Bytes(width * blockHeight * 9 / 4 + 16));

    long block = 0;
    for (long blockY = 0; blockY < height; blockY += blockHeight, block++)
    {
        if (progress((int)(blockY * 100 / height)))
            return true;
        const long ylim = std::min(blockY + blockHeight, height);
        long inp = 0;
        for (long y = blockY; y < ylim; y++)
        {
            const tjs_uint8* upper = y ? image.Line(y - 1) : nullptr;
            const tjs_uint8* current = image.Line(y);
            int prevcl[4] = {0, 0, 0, 0};
            int val[4];
            for (long x = 0; x < width; x++)
            {
                for (int c = 0; c < colors; c++)
                {
                    int cl = upper ? *current++ - *upper++ : *current++;
                    val[c] = cl - prevcl[c];
                    prevcl[c] = cl;
                }
                inbuf[0][inp] = (tjs_uint8)(val[0] - val[1]);
                inbuf[1][inp] = (tjs_uint8)val[1];
                inbuf[2][inp] = (tjs_uint8)(val[2] - val[1]);
                inbuf[3][inp] = (tjs_uint8)val[3];
                inp++;
            }
        }
        tjs_uint32 blockSize = 0;
        for (int c = 0; c < colors; c++)
        {
            long wrote = 0;
            compressor->Store();
            compressor->Encode(inbuf[c].data(), inp, outbuf[c].data(), wrote);
            if (wrote < inp)
            {
                out.push_back(0x00);
                PutLE32(out, (tjs_uint32)wrote);
                PutBytes(out, outbuf[c].data(), wrote);
                blockSize += wrote + 4 + 1;
            }
            else
            {
                compressor->Restore();
                out.push_back(0x01);
                PutLE32(out, (tjs_uint32)inp);
                PutBytes(out, inbuf[c].data(), inp);
                blockSize += inp + 4 + 1;
            }
        }
        SetLE32(out, blockSizePos + block * 4, blockSize);
    }
    progress(100);
    return false;
}

bool EncodeTlg5(const Image& image, const std::string& tags, Bytes& out, const ProgressFunc& func)
{
    Progress progress(func);
    out.clear();
    if (tags.empty())
        return EncodeTlg5Raw(image, out, progress);

    // TLG0.0 Structured Data Stream wrapping the raw TLG5 and a tags chunk.
    PutBytes(out, "TLG0.0\x00sds\x1a\x00", 11);
    const size_t rawLenPos = out.size();
    out.resize(out.size() + 4);
    Bytes raw;
    if (EncodeTlg5Raw(image, raw, progress))
        return true;
    SetLE32(out, rawLenPos, (tjs_uint32)raw.size());
    PutBytes(out, raw.data(), raw.size());
    PutBytes(out, "tags", 4);
    PutLE32(out, (tjs_uint32)tags.size());
    PutBytes(out, tags.data(), tags.size());
    return false;
}

//----------------------------------------------
// PNG

struct PngOptions
{
    struct XY
    {
        bool set = false;
        tjs_int x = 0, y = 0;
        tjs_uint8 unit = 0;
    };
    XY reso, offs, vpag;
    int level = Z_DEFAULT_COMPRESSION;
};

void ReadXY(ncbPropAccessor& dic, PngOptions::XY& xy, const tjs_char* x, const tjs_char* y,
            const tjs_char* unit, const tjs_char* oneUnit)
{
    if (!dic.HasValue(x) && !dic.HasValue(y))
        return;
    xy.set = true;
    xy.x = dic.getIntValue(x);
    xy.y = dic.getIntValue(y);
    xy.unit = dic.getStrValue(unit) == ttstr(oneUnit) ? 1 : 0;
}

// info is a tag dictionary (with an optional comp_lv) or a compression level.
PngOptions ReadPngOptions(const tTJSVariant* info, int defaultLevel)
{
    PngOptions options;
    options.level = defaultLevel;
    if (!info || info->Type() == tvtVoid)
        return options;
    if (info->Type() == tvtObject)
    {
        iTJSDispatch2* dict = info->AsObjectNoAddRef();
        if (!dict)
            return options;
        ncbPropAccessor dic(dict);
        ReadXY(dic, options.reso, TJS_N("reso_x"), TJS_N("reso_y"), TJS_N("reso_unit"), TJS_N("meter"));
        ReadXY(dic, options.offs, TJS_N("offs_x"), TJS_N("offs_y"), TJS_N("offs_unit"), TJS_N("micrometer"));
        ReadXY(dic, options.vpag, TJS_N("vpag_w"), TJS_N("vpag_h"), TJS_N("vpag_unit"), TJS_N("micrometer"));
        if (dic.HasValue(TJS_N("comp_lv")))
            options.level = dic.getIntValue(TJS_N("comp_lv"), Z_DEFAULT_COMPRESSION);
    }
    else
    {
        options.level = (int)info->AsInteger();
    }
    if (options.level < 0 || options.level > 9)
        options.level = Z_DEFAULT_COMPRESSION;
    return options;
}

void PutPngChunk(Bytes& out, const char* type, const Bytes& data)
{
    PutBE32(out, (tjs_uint32)data.size());
    const size_t typePos = out.size();
    PutBytes(out, type, 4);
    PutBytes(out, data.data(), data.size());
    uLong crc = crc32(0L, out.data() + typePos, (uInt)(data.size() + 4));
    PutBE32(out, (tjs_uint32)crc);
}

void PutPngXY(Bytes& out, const char* type, const PngOptions::XY& xy)
{
    if (!xy.set)
        return;
    Bytes data;
    PutBE32(data, (tjs_uint32)xy.x);
    PutBE32(data, (tjs_uint32)xy.y);
    data.push_back(xy.unit);
    PutPngChunk(out, type, data);
}

void PutPngHeader(Bytes& out, long width, long height, tjs_uint8 colorType)
{
    PutBytes(out, "\x89PNG\x0D\x0A\x1A\x0A", 8);
    Bytes ihdr;
    PutBE32(ihdr, (tjs_uint32)width);
    PutBE32(ihdr, (tjs_uint32)height);
    ihdr.push_back(8);         // bit depth
    ihdr.push_back(colorType); // 2 = RGB, 3 = palette, 6 = RGBA
    ihdr.push_back(0);         // deflate
    ihdr.push_back(0);         // adaptive filtering
    ihdr.push_back(0);         // no interlace
    PutPngChunk(out, "IHDR", ihdr);
}

inline tjs_uint8 Paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc)
        return (tjs_uint8)a;
    return (tjs_uint8)(pb <= pc ? b : c);
}

// Filters one scanline. Level 0 stores rows unfiltered; otherwise each row
// uses the filter with the smallest sum of signed residuals.
void FilterRow(const tjs_uint8* cur, const tjs_uint8* prev, size_t rowBytes, size_t bpp,
               bool adaptive, Bytes& out, Bytes (&candidates)[5])
{
    if (!adaptive)
    {
        out.push_back(0);
        PutBytes(out, cur, rowBytes);
        return;
    }
    size_t best = 0;
    unsigned long bestSum = ~0ul;
    for (int f = 0; f < 5; f++)
    {
        Bytes& c = candidates[f];
        c.resize(rowBytes);
        unsigned long sum = 0;
        for (size_t x = 0; x < rowBytes; x++)
        {
            int a = x >= bpp ? cur[x - bpp] : 0;
            int b = prev[x];
            int cc = x >= bpp ? prev[x - bpp] : 0;
            tjs_uint8 v;
            switch (f)
            {
                case 0: v = cur[x]; break;
                case 1: v = (tjs_uint8)(cur[x] - a); break;
                case 2: v = (tjs_uint8)(cur[x] - b); break;
                case 3: v = (tjs_uint8)(cur[x] - ((a + b) >> 1)); break;
                default: v = (tjs_uint8)(cur[x] - Paeth(a, b, cc)); break;
            }
            c[x] = v;
            sum += (unsigned long)std::abs((int)(signed char)v);
        }
        if (sum < bestSum)
        {
            bestSum = sum;
            best = f;
        }
    }
    out.push_back((tjs_uint8)best);
    PutBytes(out, candidates[best].data(), rowBytes);
}

// Returns true when canceled.
bool Deflate(const Bytes& in, int level, Bytes& out, Progress* progress)
{
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (deflateInit(&zs, level) != Z_OK)
        throw std::runtime_error("deflate initialize");
    const size_t step = 64 * 1024;
    size_t pos = 0;
    out.resize(deflateBound(&zs, (uLong)in.size()));
    zs.next_out = out.data();
    zs.avail_out = (uInt)out.size();
    int status = Z_OK;
    while (status == Z_OK)
    {
        if (zs.avail_in == 0 && pos < in.size())
        {
            const size_t n = std::min(step, in.size() - pos);
            zs.next_in = const_cast<Bytef*>(in.data() + pos);
            zs.avail_in = (uInt)n;
            pos += n;
            if (progress && (*progress)((int)(pos * 99 / in.size())))
            {
                deflateEnd(&zs);
                return true;
            }
        }
        if (zs.avail_out == 0)
        {
            const size_t used = out.size();
            out.resize(used + step);
            zs.next_out = out.data() + used;
            zs.avail_out = (uInt)step;
        }
        status = deflate(&zs, pos < in.size() ? Z_NO_FLUSH : Z_FINISH);
    }
    out.resize(zs.total_out);
    deflateEnd(&zs);
    if (status != Z_STREAM_END)
        throw std::runtime_error("deflate failed");
    return false;
}

bool EncodePng(const Image& image, const PngOptions& options, Bytes& out, const ProgressFunc& func)
{
    Progress progress(func);
    out.clear();

    bool alpha = false;
    for (size_t i = 3; i < image.px.size() && !alpha; i += 4)
        alpha = image.px[i] != 255;
    const size_t bpp = alpha ? 4 : 3;
    const size_t rowBytes = (size_t)image.w * bpp;

    Bytes filtered;
    filtered.reserve((rowBytes + 1) * image.h);
    Bytes prev(rowBytes, 0), cur(rowBytes), candidates[5];
    for (long y = 0; y < image.h; y++)
    {
        const tjs_uint8* p = image.Line(y);
        tjs_uint8* q = cur.data();
        for (long x = 0; x < image.w; x++, p += 4)
        {
            *q++ = p[2];
            *q++ = p[1];
            *q++ = p[0];
            if (alpha)
                *q++ = p[3];
        }
        FilterRow(cur.data(), prev.data(), rowBytes, bpp, options.level != 0, filtered, candidates);
        prev.swap(cur);
    }

    Bytes idat;
    if (Deflate(filtered, options.level, idat, &progress))
        return true;

    PutPngHeader(out, image.w, image.h, alpha ? 6 : 2);
    PutPngXY(out, "pHYs", options.reso);
    PutPngXY(out, "oFFs", options.offs);
    PutPngXY(out, "vpAg", options.vpag);
    PutPngChunk(out, "IDAT", idat);
    PutPngChunk(out, "IEND", Bytes());
    progress(100);
    return false;
}

// 8-bit indexed PNG with the default 3-3-2 palette (index 0 transparent).
void EncodeProvincePng(const tjs_uint8* buffer, long pitch, long width, long height, Bytes& out)
{
    static const tjs_uint8 table2bit[4] = {0, 128, 192, 255};
    static const tjs_uint8 table3bit[8] = {0, 64, 96, 128, 160, 192, 224, 255};
    Bytes palette;
    for (int i = 0; i < 256; i++)
    {
        palette.push_back(table3bit[(i >> 2) & 0x7]);
        palette.push_back(table3bit[(i >> 5) & 0x7]);
        palette.push_back(table2bit[i & 0x3]);
    }
    Bytes filtered;
    filtered.reserve((size_t)(width + 1) * height);
    for (long y = 0; y < height; y++)
    {
        filtered.push_back(0);
        PutBytes(filtered, buffer + pitch * y, width);
    }
    Bytes idat;
    Deflate(filtered, Z_DEFAULT_COMPRESSION, idat, nullptr);

    out.clear();
    PutPngHeader(out, width, height, 3);
    PutPngChunk(out, "PLTE", palette);
    PutPngChunk(out, "tRNS", Bytes(1, 0));
    PutPngChunk(out, "IDAT", idat);
    PutPngChunk(out, "IEND", Bytes());
}

//----------------------------------------------
// Storage

void WriteStorage(const ttstr& name, const Bytes& data)
{
    tTJSBinaryStream* out = TVPCreateStream(TVPNormalizeStorageName(name), TJS_BS_WRITE);
    if (!out)
        TVPThrowExceptionMessage(TJS_N("%1:can't open"), name);
    try
    {
        out->WriteBuffer(data.data(), (tjs_uint)data.size());
    }
    catch (...)
    {
        delete out;
        throw;
    }
    delete out;
}

bool IsPngName(const ttstr& name)
{
    return TVPExtractStorageExt(name).AsLowerCase() == TJS_N(".png");
}

//----------------------------------------------
// Async save

struct SaveJob
{
    bool png = false;
    PngOptions pngOptions;
    std::string tags;
    Image image;
    std::atomic<bool> canceled{false};
    // Written by the worker, read on the main thread after the done event.
    Bytes encoded;
    bool failed = false;
    std::string error;
};

struct SaveEvent
{
    std::shared_ptr<SaveJob> job;
    bool done;
    int percent;
};

struct SaveQueue
{
    std::mutex mutex;
    std::deque<SaveEvent> events;
    void Push(const std::shared_ptr<SaveJob>& job, bool done, int percent)
    {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(SaveEvent{job, done, percent});
    }
    bool Pop(SaveEvent& event)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (events.empty())
            return false;
        event = events.front();
        events.pop_front();
        return true;
    }
};

void RunSaveJob(std::shared_ptr<SaveJob> job, std::shared_ptr<SaveQueue> queue)
{
    ProgressFunc progress = [&](int percent) {
        queue->Push(job, false, percent);
        return job->canceled.load();
    };
    try
    {
        bool canceled = job->png ? EncodePng(job->image, job->pngOptions, job->encoded, progress)
                                 : EncodeTlg5(job->image, job->tags, job->encoded, progress);
        if (canceled)
            job->canceled = true;
    }
    catch (const std::exception& e)
    {
        job->failed = true;
        job->error = e.what();
    }
    catch (...)
    {
        job->failed = true;
        job->error = "unknown error";
    }
    job->image = Image();
    queue->Push(job, true, 100);
}

} // namespace

/**
 * Window extension: startSaveLayerImage / cancelSaveLayerImage /
 * stopSaveLayerImage, with onSaveLayerImageProgress(handler, progress, layer,
 * filename) and onSaveLayerImageDone(handler, canceled, layer, filename).
 */
class WindowSaveImage : public tTVPContinuousEventCallbackIntf
{
    struct Slot
    {
        std::shared_ptr<SaveJob> job;
        tTJSVariant layer;
        tTJSVariant filename;
        int percent = -1;
    };

    iTJSDispatch2* objthis;
    std::vector<Slot> slots;
    std::shared_ptr<SaveQueue> queue = std::make_shared<SaveQueue>();
    bool hooked = false;

    void SetHook(bool enable)
    {
        if (enable == hooked)
            return;
        hooked = enable;
        if (enable)
            TVPAddContinuousEventHook(this);
        else
            TVPRemoveContinuousEventHook(this);
    }

    int FindSlot(const std::shared_ptr<SaveJob>& job) const
    {
        for (size_t i = 0; i < slots.size(); i++)
            if (slots[i].job == job)
                return (int)i;
        return -1;
    }

    bool HasJobs() const
    {
        for (const Slot& slot : slots)
            if (slot.job)
                return true;
        return false;
    }

    void Fire(const tjs_char* name, tjs_int handler, tjs_int value, tTJSVariant& layer,
              tTJSVariant& filename)
    {
        tTJSVariant vhandler(handler), vvalue(value);
        tTJSVariant* args[] = {&vhandler, &vvalue, &layer, &filename};
        objthis->FuncCall(0, name, NULL, NULL, 4, args, objthis);
    }

public:
    explicit WindowSaveImage(iTJSDispatch2* objthis) : objthis(objthis) {}

    ~WindowSaveImage()
    {
        SetHook(false);
        for (Slot& slot : slots)
            if (slot.job)
                slot.job->canceled = true;
    }

    // Events are taken one at a time so a script exception leaves the rest
    // queued for the next callback.
    void OnContinuousCallback(tjs_uint64) override
    {
        SaveEvent event;
        while (queue->Pop(event))
        {
            int handler = FindSlot(event.job);
            if (handler < 0)
                continue;
            Slot& slot = slots[handler];
            if (!event.done)
            {
                if (event.percent == slot.percent)
                    continue;
                slot.percent = event.percent;
                tTJSVariant layer = slot.layer, filename = slot.filename;
                Fire(TJS_N("onSaveLayerImageProgress"), handler, event.percent, layer, filename);
                continue;
            }
            std::shared_ptr<SaveJob> job = slot.job;
            tTJSVariant layer = slot.layer, filename = slot.filename;
            slots[handler] = Slot();
            bool canceled = job->canceled || job->failed;
            if (job->failed)
                TVPAddLog(ttstr(TJS_N("layerExSave: failed to encode ")) + ttstr(filename) +
                          TJS_N(": ") + ttstr(job->error));
            if (!canceled)
            {
                try
                {
                    WriteStorage(ttstr(filename), job->encoded);
                }
                catch (...)
                {
                    TVPAddLog(ttstr(TJS_N("layerExSave: failed to write ")) + ttstr(filename));
                    canceled = true;
                }
            }
            Fire(TJS_N("onSaveLayerImageDone"), handler, canceled ? 1 : 0, layer, filename);
        }
        if (!HasJobs())
            SetHook(false);
    }

    static tjs_error startSaveLayerImage(tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                                         WindowSaveImage* self)
    {
        if (numparams < 2)
            return TJS_E_BADPARAMCOUNT;
        iTJSDispatch2* lay = param[0]->AsObjectNoAddRef();
        ttstr filename = *param[1];
        const tTJSVariant* info = numparams > 2 ? param[2] : nullptr;

        auto job = std::make_shared<SaveJob>();
        if (!Snapshot(lay, job->image))
            TVPThrowExceptionMessage(TJS_N("%1:invalid layer"), filename);
        job->png = IsPngName(filename);
        if (job->png)
            job->pngOptions = ReadPngOptions(info, Z_DEFAULT_COMPRESSION);
        else if (info && info->Type() == tvtObject)
            job->tags = BuildTlgTags(info->AsObjectNoAddRef());

        size_t handler = 0;
        while (handler < self->slots.size() && self->slots[handler].job)
            handler++;
        if (handler == self->slots.size())
            self->slots.emplace_back();
        Slot& slot = self->slots[handler];
        slot.job = job;
        slot.layer = *param[0];
        slot.filename = filename;
        slot.percent = -1;
        self->SetHook(true);
        try
        {
            std::thread(RunSaveJob, job, self->queue).detach();
        }
        catch (...)
        {
            self->slots[handler] = Slot();
            throw;
        }
        if (result)
            *result = (tjs_int)handler;
        return TJS_S_OK;
    }

    // The done event still arrives, with canceled = 1.
    void cancelSaveLayerImage(tjs_int handler)
    {
        if (handler >= 0 && handler < (tjs_int)slots.size() && slots[handler].job)
            slots[handler].job->canceled = true;
    }

    // No done event arrives for a stopped save.
    void stopSaveLayerImage(tjs_int handler)
    {
        if (handler >= 0 && handler < (tjs_int)slots.size() && slots[handler].job)
        {
            slots[handler].job->canceled = true;
            slots[handler] = Slot();
        }
    }
};

NCB_GET_INSTANCE_HOOK(WindowSaveImage)
{
    NCB_INSTANCE_GETTER(objthis)
    {
        ClassT* obj = GetNativeInstance(objthis);
        if (!obj)
            SetNativeInstance(objthis, (obj = new ClassT(objthis)));
        return obj;
    }
};

NCB_ATTACH_CLASS_WITH_HOOK(WindowSaveImage, Window)
{
    NCB_METHOD_RAW_CALLBACK(startSaveLayerImage, &WindowSaveImage::startSaveLayerImage, 0);
    NCB_METHOD(cancelSaveLayerImage);
    NCB_METHOD(stopSaveLayerImage);
}

//----------------------------------------------
// Layer save functions (synchronous)

static tjs_error saveLayerImageTlg5(tTJSVariant* result, tjs_int numparams,
                                                    tTJSVariant** param, iTJSDispatch2* lay)
{
    if (numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    ttstr filename = *param[0];
    Image image;
    if (!Snapshot(lay, image))
        TVPThrowExceptionMessage(TJS_N("%1:invalid layer"), filename);
    std::string tags;
    if (numparams > 1 && param[1]->Type() == tvtObject)
        tags = BuildTlgTags(param[1]->AsObjectNoAddRef());
    Bytes out;
    EncodeTlg5(image, tags, out, ProgressFunc());
    WriteStorage(filename, out);
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(saveLayerImageTlg5, Layer, saveLayerImageTlg5);

static tjs_error saveLayerImagePng(tTJSVariant* result, tjs_int numparams,
                                                   tTJSVariant** param, iTJSDispatch2* lay)
{
    if (numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    ttstr filename = *param[0];
    Image image;
    if (!Snapshot(lay, image))
        TVPThrowExceptionMessage(TJS_N("%1:invalid layer"), filename);
    Bytes out;
    EncodePng(image, ReadPngOptions(numparams > 1 ? param[1] : nullptr, Z_DEFAULT_COMPRESSION), out,
              ProgressFunc());
    WriteStorage(filename, out);
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(saveLayerImagePng, Layer, saveLayerImagePng);

// Returns "" when the layer has no image, as upstream does.
static tjs_error saveLayerImagePngOctet(tTJSVariant* result, tjs_int numparams,
                                                        tTJSVariant** param, iTJSDispatch2* lay)
{
    if (!result)
        return TJS_S_OK;
    *result = TJS_N("");
    Image image;
    if (!Snapshot(lay, image))
        return TJS_S_OK;
    Bytes out;
    EncodePng(image, ReadPngOptions(numparams > 0 ? param[0] : nullptr, 1), out, ProgressFunc());
    *result = tTJSVariant(out.data(), (tjs_uint)out.size());
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(saveLayerImagePngOctet, Layer, saveLayerImagePngOctet);

static tjs_error saveProvinceImage(tTJSVariant* result, tjs_int numparams,
                                                   tTJSVariant** param, iTJSDispatch2* lay)
{
    if (numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    ttstr filename = *param[0];
    long w, h;
    if (!GetLayerSize(lay, w, h))
        TVPThrowExceptionMessage(TJS_N("%1:invalid layer"), filename);
    tTJSVariant val;
    if (TJS_FAILED(lay->PropGet(0, TJS_N("provinceImageBuffer"), 0, &val, lay)))
        TVPThrowExceptionMessage(TJS_N("no province image"));
    const tjs_uint8* buffer = reinterpret_cast<const tjs_uint8*>((tjs_intptr_t)val.AsInteger());
    val.Clear();
    if (!buffer || TJS_FAILED(lay->PropGet(0, TJS_N("provinceImageBufferPitch"), 0, &val, lay)))
        TVPThrowExceptionMessage(TJS_N("no province image"));
    Bytes out;
    EncodeProvincePng(buffer, (long)val.AsInteger(), w, h, out);
    WriteStorage(filename, out);
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(saveProvinceImage, Layer, saveProvinceImage);

//----------------------------------------------
// Layer utilities

static void MakeRect(tTJSVariant* result, long x, long y, long w, long h)
{
    ncbDictionaryAccessor dict;
    dict.SetValue(TJS_N("x"), (tjs_int)x);
    dict.SetValue(TJS_N("y"), (tjs_int)y);
    dict.SetValue(TJS_N("w"), (tjs_int)w);
    dict.SetValue(TJS_N("h"), (tjs_int)h);
    *result = dict;
}

// Shrinks [0,w)x[0,h) to the smallest rectangle whose border holds a pixel
// for which hit(x, y) is true. Returns false when no pixel hits.
template <typename Hit>
static bool FindRect(long w, long h, Hit hit, long& x1, long& y1, long& rw, long& rh)
{
    auto column = [&](long x, long top, long bottom) {
        for (long y = top; y <= bottom; y++)
            if (hit(x, y))
                return true;
        return false;
    };
    auto row = [&](long y, long left, long right) {
        for (long x = left; x <= right; x++)
            if (hit(x, y))
                return true;
        return false;
    };
    long x2 = w - 1, y2 = h - 1;
    for (x1 = 0; x1 < w; x1++)
        if (column(x1, 0, h - 1))
            break;
    if (x1 >= w)
        return false;
    for (; x2 > x1; x2--)
        if (column(x2, 0, h - 1))
            break;
    for (y1 = 0; y1 < h; y1++)
        if (row(y1, x1, x2))
            break;
    for (; y2 > y1; y2--)
        if (row(y2, x1, x2))
            break;
    rw = x2 - x1 + 1;
    rh = y2 - y1 + 1;
    return true;
}

static tjs_error CropRect(tTJSVariant* result, iTJSDispatch2* lay, bool zero)
{
    LayerPixels pixels;
    OpenOrThrow(pixels, lay, false, "layerExSave.getCropRect");
    if (!result)
        return TJS_S_OK;
    result->Clear();
    long x, y, w, h;
    auto hit = [&](long px, long py) {
        const tjs_uint8* p = pixels.Line(py) + px * 4;
        return zero ? (p[0] | p[1] | p[2] | p[3]) != 0 : p[3] != 0;
    };
    if (FindRect(pixels.w, pixels.h, hit, x, y, w, h))
        MakeRect(result, x, y, w, h);
    return TJS_S_OK;
}

// Layer.getCropRect(): bounds of pixels with alpha != 0, or void.
static tjs_error getCropRect(tTJSVariant* result, tjs_int, tTJSVariant**,
                                             iTJSDispatch2* lay)
{
    return CropRect(result, lay, false);
}
NCB_ATTACH_FUNCTION(getCropRect, Layer, getCropRect);

// Layer.getCropRectZero(): bounds of pixels other than 0x00000000, or void.
static tjs_error getCropRectZero(tTJSVariant* result, tjs_int, tTJSVariant**,
                                                 iTJSDispatch2* lay)
{
    return CropRect(result, lay, true);
}
NCB_ATTACH_FUNCTION(getCropRectZero, Layer, getCropRectZero);

// Fully transparent pixels compare equal whatever their color.
static inline bool SameColor(const tjs_uint8* a, const tjs_uint8* b)
{
    return a[3] == b[3] && (a[3] == 0 || (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]));
}

// Layer.getDiffRect(base): bounds of pixels that differ from base, or void.
static tjs_error getDiffRect(tTJSVariant* result, tjs_int numparams,
                                             tTJSVariant** param, iTJSDispatch2* lay)
{
    if (numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    iTJSDispatch2* base = param[0]->AsObjectNoAddRef();
    LayerPixels target, source;
    OpenOrThrow(target, lay, false, "layerExSave.getDiffRect");
    if (base != lay)
        OpenOrThrow(source, base, false, "layerExSave.getDiffRect");
    const LayerPixels& from = base != lay ? source : target;
    if (target.w != from.w || target.h != from.h)
        TVPThrowExceptionMessage(TJS_N("Different layer size."));
    if (!result)
        return TJS_S_OK;
    result->Clear();
    long x, y, w, h;
    auto hit = [&](long px, long py) {
        return !SameColor(target.Line(py) + px * 4, from.Line(py) + px * 4);
    };
    if (FindRect(target.w, target.h, hit, x, y, w, h))
        MakeRect(result, x, y, w, h);
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(getDiffRect, Layer, getDiffRect);

// Layer.getDiffPixel(base, samecol, diffcol): fills same/different pixels
// (void leaves them) and returns the number of different pixels.
static tjs_error getDiffPixel(tTJSVariant* result, tjs_int numparams,
                                              tTJSVariant** param, iTJSDispatch2* lay)
{
    if (numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    const bool sameFill = numparams >= 2 && param[1]->Type() != tvtVoid;
    const bool diffFill = numparams >= 3 && param[2]->Type() != tvtVoid;
    const tjs_uint32 sameColor = sameFill ? (tjs_uint32)param[1]->AsInteger() : 0;
    const tjs_uint32 diffColor = diffFill ? (tjs_uint32)param[2]->AsInteger() : 0;
    iTJSDispatch2* base = param[0]->AsObjectNoAddRef();

    tTVInteger count = 0;
    if (base == lay)
    {
        // Every pixel matches itself.
        if (sameFill)
        {
            LayerPixels target;
            OpenOrThrow(target, lay, true, "layerExSave.getDiffPixel");
            for (long y = 0; y < target.h; y++)
                for (long x = 0; x < target.w; x++)
                    std::memcpy(target.Line(y) + x * 4, &sameColor, 4);
        }
        if (result)
            *result = count;
        return TJS_S_OK;
    }
    LayerPixels source, target;
    OpenOrThrow(source, base, false, "layerExSave.getDiffPixel");
    OpenOrThrow(target, lay, sameFill || diffFill, "layerExSave.getDiffPixel");
    if (target.w != source.w || target.h != source.h)
        TVPThrowExceptionMessage(TJS_N("Different layer size."));
    for (long y = 0; y < target.h; y++)
    {
        const tjs_uint8* s = source.Line(y);
        tjs_uint8* t = target.Line(y);
        for (long x = 0; x < target.w; x++, s += 4, t += 4)
        {
            if (SameColor(s, t))
            {
                if (sameFill)
                    std::memcpy(t, &sameColor, 4);
            }
            else
            {
                if (diffFill)
                    std::memcpy(t, &diffColor, 4);
                count++;
            }
        }
    }
    if (result)
        *result = count;
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(getDiffPixel, Layer, getDiffPixel);

// Layer.oozeColor(level, threshold=1, fillColor=0): spreads edge colors into
// pixels with alpha below threshold, so shrinking does not bleed fill color.
static tjs_error oozeColor(tTJSVariant* result, tjs_int numparams,
                                           tTJSVariant** param, iTJSDispatch2* lay)
{
    if (numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    const int level = (int)param[0]->AsInteger();
    if (level <= 0)
        TVPThrowExceptionMessage(TJS_N("Invalid level count."));
    const int threshold = std::max(1, std::min(255, numparams > 1 ? (int)param[1]->AsInteger() : 1));
    const tjs_uint32 fillColor = numparams > 2 ? (tjs_uint32)param[2]->AsInteger() : 0;
    const tjs_uint8 fillR = (fillColor >> 16) & 0xff, fillG = (fillColor >> 8) & 0xff, fillB = fillColor & 0xff;

    LayerPixels pixels;
    OpenOrThrow(pixels, lay, true, "layerExSave.oozeColor");
    const long w = pixels.w, h = pixels.h, ow = w + 2;
    // -1 = colored, 1 = colored this pass, 0 = not yet. int8_t, because plain
    // char is unsigned on ARM and upstream's "< 0" test never fires there.
    std::vector<int8_t> map((size_t)ow * (h + 2), 0);
    int8_t* top = map.data() + ow + 1;

    for (long y = 0; y < h; y++)
    {
        int8_t* o = top + y * ow;
        tjs_uint8* p = pixels.Line(y);
        for (long x = 0; x < w; x++, o++, p += 4)
        {
            if (p[3] >= threshold)
                *o = -1;
            else
            {
                p[2] = fillR;
                p[1] = fillG;
                p[0] = fillB;
            }
        }
    }
    for (int i = 0; i < level; i++)
    {
        for (long y = 0; y < h; y++)
        {
            int8_t* o = top + y * ow;
            tjs_uint8* p = pixels.Line(y);
            for (long x = 0; x < w; x++, o++, p += 4)
            {
                if (*o)
                    continue;
                const bool up = o[-ow] < 0, down = o[ow] < 0, left = o[-1] < 0, right = o[1] < 0;
                if (!(up || down || left || right))
                    continue;
                unsigned r = 0, g = 0, b = 0, count = 0;
                auto add = [&](const tjs_uint8* q) {
                    r += q[2];
                    g += q[1];
                    b += q[0];
                    count++;
                };
                if (up)
                    add(p - pixels.pitch);
                if (down)
                    add(p + pixels.pitch);
                if (left)
                    add(p - 4);
                if (right)
                    add(p + 4);
                p[2] = (tjs_uint8)(r / count);
                p[1] = (tjs_uint8)(g / count);
                p[0] = (tjs_uint8)(b / count);
                *o = 1;
            }
        }
        for (int8_t& o : map)
            if (o > 0)
                o = -1;
    }
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(oozeColor, Layer, oozeColor);

// Layer.copyBlueToAlpha(src): copies src's blue channel into this alpha.
static tjs_error copyBlueToAlpha(tTJSVariant* result, tjs_int numparams,
                                                 tTJSVariant** param, iTJSDispatch2* lay)
{
    if (numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    iTJSDispatch2* src = param[0]->AsObjectNoAddRef();
    LayerPixels source, target;
    if (src != lay)
        OpenOrThrow(source, src, false, "layerExSave.copyBlueToAlpha", TJS_N("src must be Layer."));
    OpenOrThrow(target, lay, true, "layerExSave.copyBlueToAlpha", TJS_N("dest must be Layer."));
    const LayerPixels& from = src != lay ? source : target;
    const long w = std::min(from.w, target.w), h = std::min(from.h, target.h);
    for (long y = 0; y < h; y++)
    {
        const tjs_uint8* p = from.Line(y);
        tjs_uint8* q = target.Line(y) + 3;
        for (long x = 0; x < w; x++, p += 4, q += 4)
            *q = *p;
    }
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(copyBlueToAlpha, Layer, copyBlueToAlpha);

// Clips (left, top, width, height) to the image. Returns false when empty.
static bool ClipRect(tTJSVariant** param, long w, long h, long& left, long& top, long& width, long& height)
{
    left = (long)param[0]->AsInteger();
    top = (long)param[1]->AsInteger();
    width = (long)param[2]->AsInteger();
    height = (long)param[3]->AsInteger();
    if (left < 0)
    {
        width += left;
        left = 0;
    }
    if (top < 0)
    {
        height += top;
        top = 0;
    }
    width = std::min(width, w - left);
    height = std::min(height, h - top);
    return width > 0 && height > 0;
}

// Layer.isBlank(x, y, w, h): true when every pixel in the area is 0x00000000.
// Upstream tested only the blue byte of each pixel.
static tjs_error isBlank(tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
                                         iTJSDispatch2* lay)
{
    if (numparams < 4)
        return TJS_E_BADPARAMCOUNT;
    LayerPixels pixels;
    OpenOrThrow(pixels, lay, false, "layerExSave.isBlank", TJS_N("src must be Layer."));
    long left, top, width, height;
    bool blank = true;
    if (ClipRect(param, pixels.w, pixels.h, left, top, width, height))
    {
        for (long y = top; y < top + height && blank; y++)
        {
            const tjs_uint8* p = pixels.Line(y) + left * 4;
            for (long x = 0; x < width * 4; x++)
                if (p[x])
                {
                    blank = false;
                    break;
                }
        }
    }
    if (result)
        *result = (tjs_int)(blank ? 1 : 0);
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(isBlank, Layer, isBlank);

// Layer.clearAlpha(threshold=0, fillColor=0): pixels with alpha <= threshold
// become fillColor (0x00RRGGBB) with alpha 0.
static tjs_error clearAlpha(tTJSVariant* result, tjs_int numparams,
                                            tTJSVariant** param, iTJSDispatch2* lay)
{
    const int threshold = numparams > 0 ? (int)param[0]->AsInteger() : 0;
    const tjs_uint32 fillColor = (tjs_uint32)(numparams > 1 ? param[1]->AsInteger() : 0) & 0xffffff;
    LayerPixels pixels;
    OpenOrThrow(pixels, lay, true, "layerExSave.clearAlpha", TJS_N("dest must be Layer."));
    for (long y = 0; y < pixels.h; y++)
    {
        tjs_uint8* p = pixels.Line(y);
        for (long x = 0; x < pixels.w; x++, p += 4)
            if (p[3] <= threshold)
                std::memcpy(p, &fillColor, 4);
    }
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(clearAlpha, Layer, clearAlpha);

// Layer.getAverageColor(left, top, width, height). The byte order matches
// upstream (average B in the top byte, then G, R, A), which scripts written
// against the original plugin expect.
static tjs_error getAverageColor(tTJSVariant* result, tjs_int numparams,
                                                 tTJSVariant** param, iTJSDispatch2* lay)
{
    if (numparams < 4)
        return TJS_E_BADPARAMCOUNT;
    LayerPixels pixels;
    OpenOrThrow(pixels, lay, false, "layerExSave.getAverageColor", TJS_N("src must be Layer."));
    long left, top, width, height;
    if (!ClipRect(param, pixels.w, pixels.h, left, top, width, height))
        TVPThrowExceptionMessage(TJS_N("invalid layer range"));
    double sum[4] = {0, 0, 0, 0};
    for (long y = top; y < top + height; y++)
    {
        const tjs_uint8* p = pixels.Line(y) + left * 4;
        for (long x = 0; x < width; x++, p += 4)
            for (int c = 0; c < 4; c++)
                sum[c] += p[c];
    }
    const double size = (double)width * height;
    tjs_uint32 color = 0;
    for (int c = 0; c < 4; c++)
        color = (color << 8) | ((tjs_uint32)(sum[c] / size) & 0xff);
    if (result)
        *result = (tjs_int)color;
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(getAverageColor, Layer, getAverageColor);

// Layer.getFingerPrintValue(ignore_transparent=true): 64-bit FNV-1a of the
// pixels and the image size. Equal hashes still need an exact comparison.
static tjs_error getFingerPrintValue(tTJSVariant* result, tjs_int numparams,
                                                     tTJSVariant** param, iTJSDispatch2* lay)
{
    LayerPixels pixels;
    OpenOrThrow(pixels, lay, false, "layerExSave.getFingerPrintValue");
    const bool lazy = numparams > 0 && param[0]->Type() != tvtVoid ? (bool)param[0]->operator bool() : true;
    tjs_uint64 hash = 0xCBF29CE484222325ull;
    auto mix = [&](tjs_uint8 n) { hash = (hash ^ n) * 0x00000100000001B3ull; };
    for (long y = 0; y < pixels.h; y++)
    {
        const tjs_uint8* p = pixels.Line(y);
        for (long x = 0; x < pixels.w; x++, p += 4)
        {
            const bool zero = lazy && !p[3];
            for (int c = 0; c < 4; c++)
                mix(zero ? 0 : p[c]);
        }
    }
    for (int i = 24; i >= 0; i -= 8)
    {
        mix((pixels.w >> i) & 255);
        mix((pixels.h >> i) & 255);
    }
    if (result)
        *result = (tTVInteger)hash;
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(getFingerPrintValue, Layer, getFingerPrintValue);

// Layer.getShrinkVectorOctet(w=16, h=16): per-block average BGRA (opaque
// pixels only contribute color) as an octet of w*h*4 bytes.
static tjs_error getShrinkVectorOctet(tTJSVariant* result, tjs_int numparams,
                                                      tTJSVariant** param, iTJSDispatch2* lay)
{
    const long svw = numparams > 0 && param[0]->Type() != tvtVoid ? (long)param[0]->AsInteger() : 16;
    const long svh = numparams > 1 && param[1]->Type() != tvtVoid ? (long)param[1]->AsInteger() : 16;
    if (svw <= 0 || svh <= 0)
        return TJS_E_INVALIDPARAM;
    LayerPixels pixels;
    OpenOrThrow(pixels, lay, false, "layerExSave.getShrinkVectorOctet");
    const long w = pixels.w, h = pixels.h;
    const long stepW = (w + svw - 1) / svw, stepH = (h + svh - 1) / svh;
    Bytes vector;
    for (long y = 0; y < h; y += stepH)
    {
        const long bh = std::min(stepH, h - y);
        for (long x = 0; x < w; x += stepW)
        {
            const long bw = std::min(stepW, w - x);
            tjs_uint32 s[4] = {0, 0, 0, 0}, total = 0;
            for (long by = 0; by < bh; by++)
            {
                const tjs_uint8* p = pixels.Line(y + by) + x * 4;
                for (long bx = 0; bx < bw; bx++, p += 4, total++)
                    if (p[3])
                        for (int c = 0; c < 4; c++)
                            s[c] += p[c];
            }
            for (int c = 0; c < 4; c++)
                vector.push_back((tjs_uint8)((s[c] + total / 2) / total));
        }
    }
    if (result)
        *result = tTJSVariant(vector.data(), (tjs_uint)vector.size());
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(getShrinkVectorOctet, Layer, getShrinkVectorOctet);

// Math.octetVectorSum(a, b=void): [ Σab, Σ(a-b)², Σa², Σb², Σa, Σb, count(a!=b) ].
// With b void: [ 0, Σa², Σa², 0, Σa, 0, count(a!=0) ].
static tjs_error octetVectorSum(tTJSVariant* result, tjs_int numparams,
                                                tTJSVariant** param, iTJSDispatch2*)
{
    if (numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    if (param[0]->Type() != tvtOctet)
        return TJS_E_INVALIDPARAM;
    tTJSVariantOctet* a = param[0]->AsOctetNoAddRef();
    tTJSVariantOctet* b = numparams > 1 && param[1]->Type() == tvtOctet ? param[1]->AsOctetNoAddRef() : nullptr;
    if (!a || (b && a->GetLength() != b->GetLength()))
        return TJS_E_INVALIDPARAM;
    const tjs_uint8* pa = a->GetData();
    const tjs_uint8* pb = b ? b->GetData() : nullptr;
    const tjs_uint length = a->GetLength();
    if (length && (!pa || (b && !pb)))
        return TJS_E_INVALIDPARAM;

    tTVInteger csum = 0, dsum = 0, nsum1 = 0, nsum2 = 0, psum1 = 0, psum2 = 0, diff = 0;
    for (tjs_uint n = 0; n < length; n++)
    {
        const tTVInteger va = pa[n], vb = pb ? pb[n] : 0;
        const tTVInteger vd = va > vb ? va - vb : vb - va;
        csum += va * vb;
        dsum += vd * vd;
        nsum1 += va;
        nsum2 += vb;
        psum1 += va * va;
        psum2 += vb * vb;
        if (va != vb)
            diff++;
    }
    if (result)
    {
        iTJSDispatch2* array = TJSCreateArrayObject();
        const tTVInteger values[] = {csum, dsum, psum1, psum2, nsum1, nsum2, diff};
        for (tjs_int i = 0; i < 7; i++)
        {
            tTJSVariant v(values[i]);
            array->PropSetByNum(TJS_MEMBERENSURE, i, &v, array);
        }
        *result = tTJSVariant(array, array);
        array->Release();
    }
    return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(octetVectorSum, Math, octetVectorSum);
