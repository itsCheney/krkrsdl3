#pragma once

#include <cstring>
#include <utility>
#include <vector>

namespace emoteplayer
{
template<class Frame>
std::pair<Frame*, Frame*> selectEmoteNodeFrames(const std::vector<Frame*>& frames,
                                               double tick, bool shapeNode)
{
    Frame* current = nullptr;
    Frame* next = nullptr;
    for (auto* frame : frames)
    {
        if (frame->time > tick) { next = frame; break; }
        current = frame;
    }
    // A terminal empty frame ends visual playback. Non-drawing containers and
    // hit shapes retain geometry, so static hit areas keep their parent transform.
    // Never resurrect icons hidden by earlier empty keys.
    if (current && !current->hasContent && !next)
    {
        for (auto it = frames.rbegin(); it != frames.rend(); ++it)
        {
            auto* previous = *it;
            if (!previous->hasContent) continue;
            if (shapeNode || std::strcmp(previous->src.c_str(), "layout") == 0 ||
                             std::strcmp(previous->src.c_str(), "clip") == 0 ||
                             previous->src.rfind("blank/", 0) == 0 ||
                             previous->src.rfind("src/blank/", 0) == 0 ||
                             previous->src.rfind("shape/", 0) == 0 ||
                             previous->src.rfind("src/shape/", 0) == 0)
                current = previous;
            break;
        }
    }
    if (next && !next->hasContent) next = nullptr;
    return {current, next};
}
} // namespace emoteplayer
