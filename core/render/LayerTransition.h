#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

class iTVPTexture2D;

enum class TVPLayerTransitionKind : std::int32_t {
#define TVP_LAYER_TRANSITION(name, id, text) name = id,
#define TVP_LAYER_TRANSITION_COUNT(count) Count = count
#include "LayerTransitionDefinitions.def"
#undef TVP_LAYER_TRANSITION_COUNT
#undef TVP_LAYER_TRANSITION
};

inline const char *TVPLayerTransitionName(TVPLayerTransitionKind kind) {
    switch(kind) {
#define TVP_LAYER_TRANSITION(name, id, text) case TVPLayerTransitionKind::name: return text;
#define TVP_LAYER_TRANSITION_COUNT(count)
#include "LayerTransitionDefinitions.def"
#undef TVP_LAYER_TRANSITION_COUNT
#undef TVP_LAYER_TRANSITION
    default: return "unknown";
    }
}

// Exactly the compute uniform layout. Canvas dimensions belong to the handler;
// source and destination textures can be larger. Rectangles are half-open.
struct TVPLayerTransitionParams {
    std::int32_t kind = 0, frameWidth = 0, frameHeight = 0, ratio = 0;
    std::uint32_t flags = 0, color = 0;
    std::int32_t blockSize = 0, offsetX = 0, offsetY = 0, phase = 0;
    std::int32_t centerX = 0, centerY = 0, mapWidth = 0, mapHeight = 0, driftOffset = 0;
    std::int32_t left = 0, top = 0, width = 0, height = 0, destLeft = 0, destTop = 0;
};
static_assert(sizeof(TVPLayerTransitionParams) == 21 * 4, "transition uniform ABI");

using TVPLayerTransitionBytes = std::vector<std::uint8_t>;
struct TVPLayerTransitionOperation {
    TVPLayerTransitionParams params;
    // Owned immutable bytes outlive command encoding and can be retained by a
    // backend cache. Identity plus version identifies cached table contents.
    std::shared_ptr<const TVPLayerTransitionBytes> table, rows;
    std::uint64_t tableVersion = 1, rowsVersion = 1;
};

enum class TVPLayerTransitionResult {
    Applied, Unsupported, InvalidGeometry, InvalidResource, Alias, CPUAccess,
    PipelineUnavailable, AllocationFailed, InvalidParameters
};

bool TVPHasMetalLayerTransitionSupport();

// A rejection must leave target pixels unchanged. No CPU lease is opened here.
TVPLayerTransitionResult TVPTryMetalLayerTransition(
    const TVPLayerTransitionOperation &operation, iTVPTexture2D *target,
    iTVPTexture2D *source1, iTVPTexture2D *source2);

inline std::shared_ptr<const TVPLayerTransitionBytes> TVPMakeTransitionBytes(
        const void *data, std::size_t size) {
    auto bytes = std::make_shared<TVPLayerTransitionBytes>(size);
    if(size) std::memcpy(bytes->data(), data, size);
    return bytes;
}

// Buffer formats (native little-endian 32/16-bit values):
// wave rows: one int32 displacement per Process row (starts at params.top).
// turn table: 64*64 records of 8 int32 start,len,sx,sy,ex,ey,stepx,stepy,
// followed by 64 int32 gloss values. offsetX is the phase width factor.
// ripple table: mapWidth*mapHeight uint16 displacement values followed by
// the complete uint16 drift lookup table. driftOffset indexes table uint16s.
// rotate rows: frameHeight records of 26 int32: count, src1[5], src2[5],
// region[5][3]. Lines use start,sx,sy,stepx,stepy; regions left,right,type.
