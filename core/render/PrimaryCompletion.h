#pragma once
#include "ComplexRect.h"
#include "drawable.h"

namespace krkrsdl3 { namespace primary_completion {
// A totally opaque Primary hands its children the manager DrawBuffer itself
// (tTJSNI_BaseLayer::Draw, totalopaque branch). Completing that region then
// asks the manager to Blt DrawBuffer onto itself at the same coordinates:
// an opaque full-opacity copy whose result equals its input, with or without
// HoldAlpha. GPU backends still snapshot the aliased source and break the
// render pass for it, once per update rectangle. Every other combination
// (another bitmap, an offset, partial opacity or a blending type) is real work.
inline bool IsIdentity(const void* drawBuffer, const void* bitmap, const tTVPRect& destination,
                       const tTVPRect& source, tTVPLayerType type, tjs_int opacity) noexcept {
    return drawBuffer && bitmap == drawBuffer && type == ltOpaque && opacity == 255 &&
        destination.left == source.left && destination.top == source.top &&
        destination.get_width() == source.get_width() && destination.get_height() == source.get_height();
}
} }
