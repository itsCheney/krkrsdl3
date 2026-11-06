#include "tjsCommHead.h"
#include "TVPGraphicsLoader.h"
#include "TVPMsg.h"
#include "tjsDictionary.h"

#include <limits>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include <libbpg.h>
// Private helpers from the pinned decoder, never from the system FFmpeg.
void krkr_bpg_ff_init_cabac_states(void);
void krkr_bpg_hevc_transform_init(void);
}

namespace {
using BPGContext = std::unique_ptr<BPGDecoderContext, decltype(&bpg_decoder_close)>;

void InvalidBPG()
{
    TVPThrowExceptionMessage(TJS_N("Invalid BPG image"));
}

bool QuickTestBPG(tTJSBinaryStream* src)
{
    const auto position = src->GetPosition();
    tjs_uint8 signature[4] = {};
    const auto count = src->Read(signature, sizeof(signature));
    src->SetPosition(position);
    return count == sizeof(signature) && !memcmp(signature, "BPG\xfb", sizeof(signature));
}

void LoadBPG(void*, void* callbackdata, tTVPGraphicSizeCallback sizecallback,
             tTVPGraphicScanLineCallback scanlinecallback, tTVPMetaInfoPushCallback,
             tTJSBinaryStream* src, tjs_int32, tTVPGraphicLoadMode mode)
{
    if (mode == glmPalettized)
        TVPThrowExceptionMessage(TJS_N("BPG does not support palettized images"));

    const auto position = src->GetPosition();
    const auto size = src->GetSize();
    if (position > size || size - position == 0 ||
        size - position > static_cast<tjs_uint64>(std::numeric_limits<int>::max()))
        InvalidBPG();
    std::vector<uint8_t> data(static_cast<size_t>(size - position));
    src->ReadBuffer(data.data(), static_cast<tjs_uint>(data.size()));
    // Upstream's lazy shared-table initialization is not thread-safe. Finish
    // it once before synchronous and asynchronous image loads can overlap.
    static std::once_flag tablesReady;
    std::call_once(tablesReady, [] {
        krkr_bpg_ff_init_cabac_states();
        krkr_bpg_hevc_transform_init();
    });
    BPGContext decoder(bpg_decoder_open(), bpg_decoder_close);
    if (!decoder || bpg_decoder_decode(decoder.get(), data.data(), static_cast<int>(data.size())) < 0)
        InvalidBPG();
    BPGImageInfo info = {};
    if (bpg_decoder_get_info(decoder.get(), &info) < 0 ||
        bpg_decoder_start(decoder.get(), BPG_OUTPUT_FORMAT_RGBA32) < 0)
        InvalidBPG();

    sizecallback(callbackdata, info.width, info.height,
                 mode == glmGrayscale ? gpfLuminance : (info.has_alpha ? gpfRGBA : gpfRGB));
    std::vector<uint8_t> row;
    if (mode == glmGrayscale)
        row.resize(static_cast<size_t>(info.width) * 4);
    for (uint32_t y = 0; y < info.height; ++y)
    {
        auto* destination = static_cast<uint8_t*>(scanlinecallback(callbackdata, y));
        if (!destination)
            break;
        auto* pixels = mode == glmGrayscale ? row.data() : destination;
        if (bpg_decoder_get_line(decoder.get(), pixels) < 0)
            InvalidBPG();
        if (mode == glmGrayscale)
        {
            for (uint32_t x = 0; x < info.width; ++x)
                destination[x] = (pixels[x * 4] * 77 + pixels[x * 4 + 1] * 150 +
                                  pixels[x * 4 + 2] * 29 + 128) >> 8;
        }
        scanlinecallback(callbackdata, -1);
    }
}

void LoadHeaderBPG(void*, tTJSBinaryStream* src, iTJSDispatch2** dic)
{
    // Header queries do not need to allocate or decode the HEVC image.
    uint8_t header[BPG_DECODER_INFO_BUF_SIZE] = {};
    const auto count = src->Read(header, sizeof(header));
    BPGImageInfo info = {};
    if (bpg_decoder_get_info_from_buf(&info, nullptr, header, count) < 0)
        InvalidBPG();
    *dic = TJSCreateDictionaryObject();
    tTJSVariant value(static_cast<tjs_int64>(info.width));
    (*dic)->PropSet(TJS_MEMBERENSURE, TJS_N("width"), nullptr, &value, *dic);
    value = static_cast<tjs_int64>(info.height);
    (*dic)->PropSet(TJS_MEMBERENSURE, TJS_N("height"), nullptr, &value, *dic);
    value = info.has_alpha ? 32 : 24;
    (*dic)->PropSet(TJS_MEMBERENSURE, TJS_N("bpp"), nullptr, &value, *dic);
}
}

tTVPRegisterGraphicInfo _bpgGraphicInfo(TJS_N(".bpg"), QuickTestBPG, LoadBPG,
                                      LoadHeaderBPG, nullptr, nullptr, nullptr);
