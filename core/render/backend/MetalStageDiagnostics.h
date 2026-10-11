#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace krkrsdl3 { namespace metal_diagnostics { namespace stage_timing {

// Counter timestamps use the GPU's clock, not necessarily nanoseconds. Device
// sampleTimestamps supplies CPU time in nanoseconds. The paired device samples
// bracket encoding and completion of this command buffer.
struct ClockCalibration
{
    uint64_t cpuBegin = 0, gpuBegin = 0, cpuEnd = 0, gpuEnd = 0;
    bool Valid() const { return cpuEnd > cpuBegin && gpuEnd > gpuBegin; }
    double NanosecondsPerTick() const
    {
        return Valid() ? double(cpuEnd - cpuBegin) / double(gpuEnd - gpuBegin) : 0.0;
    }
};

enum class Stage : uint8_t
{
    MeshVertex, MeshFragment, WindowVertex, WindowFragment,
    OtherVertex, OtherFragment, LayerCompute, Blit, Count
};
enum class RenderKind : uint8_t { Mesh, Window, Other };
constexpr size_t StageCount = static_cast<size_t>(Stage::Count);
constexpr uint32_t MaxSamples = 512;
constexpr uint32_t NoSample = std::numeric_limits<uint32_t>::max();

// Two samples per interval. No allocation or GPU operation is performed here.
// A render reservation covers vertex and fragment stages of one existing pass;
// it does not split a reused mesh encoder or distinguish draws inside that pass.
struct Plan
{
    std::array<Stage, MaxSamples / 2> stages{};
    uint32_t sampleCount = 0;
    uint32_t droppedPasses = 0;
    uint32_t failedPasses = 0;

    uint32_t ReserveEncoder(Stage stage)
    {
        if (sampleCount + 2 > MaxSamples) { ++droppedPasses; return NoSample; }
        const uint32_t first = sampleCount;
        stages[sampleCount / 2] = stage;
        sampleCount += 2;
        return first;
    }
    uint32_t ReserveRender(RenderKind kind)
    {
        if (sampleCount + 4 > MaxSamples) { ++droppedPasses; return NoSample; }
        const Stage vertex = kind == RenderKind::Mesh ? Stage::MeshVertex :
            kind == RenderKind::Window ? Stage::WindowVertex : Stage::OtherVertex;
        const uint32_t first = ReserveEncoder(vertex);
        ReserveEncoder(static_cast<Stage>(static_cast<uint8_t>(vertex) + 1));
        return first;
    }
    // Only call for the most recent reservation when its encoder factory failed.
    // The normal factory can then encode the original pass without diagnostics.
    void Rollback(uint32_t checkpoint)
    {
        if (checkpoint <= sampleCount && checkpoint % 2 == 0) sampleCount = checkpoint;
        ++failedPasses;
    }
};

struct StageResult
{
    double milliseconds = 0;
    uint32_t valid = 0, invalid = 0;
    double Milliseconds() const { return valid ? milliseconds : -1.0; }
};
struct Summary
{
    std::array<StageResult, StageCount> stages{};
    uint32_t valid = 0, invalid = 0;
    bool calibrationValid = false;
};

// Stage intervals can overlap each other and contain scheduling gaps. These
// observations must not be summed/divided to invent a command-buffer budget.
inline Summary Resolve(const Plan& plan, const uint64_t* timestamps, size_t count,
                       const ClockCalibration& clock,
                       uint64_t errorValue = std::numeric_limits<uint64_t>::max())
{
    Summary result;
    result.calibrationValid = clock.Valid();
    const double millisecondsPerTick = clock.NanosecondsPerTick() / 1000000.0;
    for (uint32_t index = 0; index + 1 < plan.sampleCount && index + 1 < MaxSamples; index += 2) {
        auto& stage = result.stages[static_cast<size_t>(plan.stages[index / 2])];
        const bool present = timestamps && index + 1 < count;
        const uint64_t begin = present ? timestamps[index] : 0;
        const uint64_t end = present ? timestamps[index + 1] : 0;
        if (!result.calibrationValid || !begin || !end || begin == errorValue ||
            end == errorValue || end < begin || begin < clock.gpuBegin || end > clock.gpuEnd) {
            ++stage.invalid;
            ++result.invalid;
            continue;
        }
        stage.milliseconds += double(end - begin) * millisecondsPerTick;
        ++stage.valid;
        ++result.valid;
    }
    return result;
}

}}} // namespace krkrsdl3::metal_diagnostics::stage_timing
