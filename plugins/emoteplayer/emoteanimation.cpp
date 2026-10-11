// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mikage contributors.
// Adapted animation behavior from AetherKiri/AetherKrkr fa0f8af; see AETHER-NOTICE.md.
#include "emoteanimation.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace emoteplayer::animation {
namespace {
constexpr auto missing = std::numeric_limits<std::size_t>::max();
bool finite(double v) { return std::isfinite(v); }
std::string encodeLabel(const std::string& label) {
    static const char digits[] = "0123456789abcdef";
    std::string result; result.reserve(label.size() * 2);
    for (unsigned char c : label) { result += digits[c >> 4]; result += digits[c & 15]; }
    return result;
}
bool decodeLabel(std::string& label) {
    if (label.size() > 131072 || label.size() % 2) return false;
    std::string result; result.reserve(label.size() / 2);
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (std::size_t i = 0; i < label.size(); i += 2) {
        const int hi = digit(label[i]), lo = digit(label[i + 1]); if (hi < 0 || lo < 0) return false;
        result += static_cast<char>((hi << 4) | lo);
    }
    label = std::move(result); return true;
}
}
double easeWeight(double easing) {
    if (!finite(easing)) return 1;
    return easing >= 0 ? easing + 1 : 1 / (1 - easing);
}
double millisecondsToFrames(double ms, double speedDivisor) {
    if (!finite(ms) || !finite(speedDivisor) || ms <= 0 || speedDivisor <= 0) return 0;
    // Keep the existing public speed divisor (default 20); authored time is 60 Hz.
    return ms * (60.0 / 1000.0) * (20.0 / speedDivisor);
}
void Animator::assign(double v) {
    value = start = target = v; duration = elapsed = 0; weight = 1; queue.clear();
}
void Animator::schedule(double v, double time, double easing, bool queuing) {
    if (!finite(v) || !finite(time) || !finite(easing)) return;
    if (time <= 0) { assign(v); return; }
    if (!queuing) { queue.clear(); duration = elapsed = 0; }
    queue.push_back({v, time, easeWeight(easing)});
    advance(0);
}
void Animator::advance(double dt) {
    if (!finite(dt) || dt < 0) return;
    do {
        if (elapsed >= duration) {
            if (queue.empty()) return;
            const auto request = queue.front(); queue.pop_front();
            start = value; target = request.target; duration = request.duration;
            weight = request.weight; elapsed = 0;
        }
        const double consume = std::min(dt, duration - elapsed);
        elapsed = std::min(duration, elapsed + consume);
        const double p = std::clamp(elapsed / duration, 0.0, 1.0);
        value = elapsed >= duration ? target : start + (target - start) * std::pow(p, weight);
        dt = std::max(0.0, dt - consume);
    } while (dt > 0 && (elapsed < duration || !queue.empty()));
}
std::size_t Runtime::index(const std::string& label) const {
    for (std::size_t i = 0; i < definitions_.size(); ++i)
        if (definitions_[i].label == label) return i;
    return missing;
}
const Timeline* Runtime::definition(const std::string& label) const {
    const auto i = index(label); return i == missing ? nullptr : &definitions_[i];
}
const TimelineState* Runtime::state(const std::string& label) const {
    const auto i = index(label); return i == missing ? nullptr : &states_[i];
}
void Runtime::reset(std::vector<Timeline> definitions, const std::map<std::string, double>& defaults) {
    definitions_ = std::move(definitions); states_.clear(); variables_.clear(); values_.clear(); counters_ = {};
    for (auto& definition : definitions_) {
        for (auto& track : definition.tracks) {
            track.frames.erase(std::remove_if(track.frames.begin(), track.frames.end(), [](const Frame& f) {
                return !finite(f.time) || f.time < 0 || !finite(f.value) || !finite(f.easing);
            }), track.frames.end());
            std::stable_sort(track.frames.begin(), track.frames.end(), [](const Frame& a, const Frame& b) { return a.time < b.time; });
            if (!track.frames.empty()) definition.lastTime = std::max(definition.lastTime, track.frames.back().time);
        }
        TimelineState state; state.label = definition.label; state.blend.assign(1);
        state.tracks.resize(definition.tracks.size()); state.cursors.assign(definition.tracks.size(), -1);
        states_.push_back(std::move(state));
    }
    for (const auto& [label, value] : defaults) if (finite(value)) variables_[label].assign(value);
    evaluate();
}
void Runtime::cross(std::size_t i, double time) {
    auto& state = states_[i]; const auto& definition = definitions_[i];
    for (std::size_t k = 0; k < definition.tracks.size(); ++k) {
        const auto& track = definition.tracks[k]; auto& cursor = state.cursors[k];
        while (cursor + 1 < static_cast<int>(track.frames.size()) && track.frames[cursor + 1].time <= time) {
            const auto n = static_cast<std::size_t>(++cursor); const auto& frame = track.frames[n];
            ++counters_.crossings;
            if (!frame.content || ((state.flags & 4) && track.instant)) continue;
            const double duration = n + 1 < track.frames.size() ? std::max(0.0, track.frames[n + 1].time - std::max(time, frame.time) - 1.0) : 0;
            auto& animator = state.tracks[k];
            if (track.instant) animator.assign(frame.value);
            else animator.schedule(frame.value, duration, frame.easing, queuing_);
        }
    }
}
void Runtime::initialize(std::size_t i, double time, bool preserveValue) {
    auto& state = states_[i]; state.time = time;
    std::fill(state.cursors.begin(), state.cursors.end(), -1);
    for (std::size_t k = 0; k < state.tracks.size(); ++k) {
        const auto& track = definitions_[i].tracks[k];
        const auto it = variables_.find(track.label);
        const double value = preserveValue ? state.tracks[k].value : state.difference || it == variables_.end() ? 0 : it->second.value;
        if (!preserveValue || !queuing_) state.tracks[k].assign(value);
        int lastContent = -1;
        for (std::size_t n = 0; n < track.frames.size() && track.frames[n].time <= time; ++n) {
            state.cursors[k] = static_cast<int>(n);
            if (track.frames[n].content) lastContent = static_cast<int>(n);
        }
        if (lastContent < 0 || ((state.flags & 4) && track.instant)) continue;
        ++counters_.crossings;
        const auto n = static_cast<std::size_t>(lastContent); const auto& frame = track.frames[n];
        const double duration = n + 1 < track.frames.size() ? std::max(0.0, track.frames[n + 1].time - time - 1.0) : 0;
        if (track.instant) state.tracks[k].assign(frame.value);
        else state.tracks[k].schedule(frame.value, duration, frame.easing, queuing_);
    }
}
bool Runtime::play(const std::string& label, int flags) {
    const auto i = index(label); if (i == missing) return false;
    if (!(flags & 1)) stop();
    auto& state = states_[i]; state.flags = flags; state.playing = true;
    state.difference = definitions_[i].difference || (flags & 2);
    state.stopAfterBlend = false; state.blend.assign(1); initialize(i, 0); evaluate(); return true;
}
void Runtime::stop(const std::string& label) {
    for (std::size_t i = 0; i < states_.size(); ++i) {
        auto& state = states_[i];
        if (!label.empty() && state.label != label) continue;
        if (state.playing && !state.difference)
            for (std::size_t k = 0; k < state.tracks.size(); ++k)
                if (state.cursors[k] >= 0) variables_[definitions_[i].tracks[k].label].assign(state.tracks[k].value);
        state.playing = false;
    }
    evaluate();
}
bool Runtime::playing(const std::string& label) const {
    for (const auto& state : states_) if (state.playing && (label.empty() || state.label == label)) return true;
    return false;
}
bool Runtime::active() const {
    if (playing()) return true;
    for (const auto& [label, animator] : variables_) if (animator.active()) return true;
    return false;
}
void Runtime::setVariable(const std::string& label, double value, double time, double easing) {
    if (!finite(value) || !finite(time) || !finite(easing)) return;
    // Script writes interrupt the current primary controller, until an authored
    // key schedules its next request. Difference controllers remain additive.
    for (std::size_t i = 0; i < states_.size(); ++i) {
        auto& state = states_[i]; if (!state.playing || state.difference) continue;
        for (std::size_t k = 0; k < state.tracks.size(); ++k)
            if (definitions_[i].tracks[k].label == label) state.tracks[k].schedule(value, time, easing, queuing_);
    }
    variables_[label].schedule(value, time, easing, queuing_); evaluate();
}
bool Runtime::hasVariable(const std::string& label) const { return values_.find(label) != values_.end(); }
double Runtime::variable(const std::string& label) const {
    const auto it = values_.find(label); return it == values_.end() ? 0 : it->second;
}
void Runtime::setBlend(const std::string& label, double ratio, double time, double easing, bool autoStop) {
    const auto i = index(label); if (i == missing || !finite(ratio) || !finite(time) || !finite(easing)) return;
    states_[i].blend.schedule(std::clamp(ratio, 0.0, 1.0), time, easing);
    states_[i].stopAfterBlend = autoStop;
    if (autoStop && !states_[i].blend.active() && states_[i].blend.value <= 0) states_[i].playing = false;
    evaluate();
}
void Runtime::fadeIn(const std::string& label, double time, double easing) {
    if (!playing(label)) { if (!play(label, 3)) return; setBlend(label, 0, 0, 0); }
    setBlend(label, 1, time, easing);
}
void Runtime::fadeOut(const std::string& label, double time, double easing) { setBlend(label, 0, time, easing, true); }
void Runtime::advanceState(std::size_t i, double dt) {
    auto& state = states_[i]; const auto& definition = definitions_[i];
    const bool loop = definition.loopBegin >= 0 && definition.loopEnd > definition.loopBegin;
    const double end = loop ? definition.loopEnd : std::max(0.0, definition.lastTime);
    while (state.playing && dt > 0) {
        double next = end;
        if (state.stopAfterBlend && state.blend.elapsed < state.blend.duration)
            next = std::min(next, state.time + state.blend.duration - state.blend.elapsed);
        for (std::size_t k = 0; k < definition.tracks.size(); ++k) {
            const auto n = state.cursors[k] + 1;
            if (n < static_cast<int>(definition.tracks[k].frames.size()))
                next = std::min(next, definition.tracks[k].frames[n].time);
        }
        const double consume = std::min(dt, std::max(0.0, next - state.time));
        for (auto& track : state.tracks) track.advance(consume);
        state.blend.advance(consume); state.time += consume; dt = std::max(0.0, dt - consume);
        if (state.stopAfterBlend && !state.blend.active() && state.blend.value <= 0) { state.playing = false; break; }
        if (state.time >= end) {
            if (loop) {
                bool settled = true;
                for (const auto& track : state.tracks) settled &= !track.active();
                ++counters_.wraps; initialize(i, definition.loopBegin, true);
                // Whole cycles return to the same controller initial state.
                // Preserve blend time and remainder without iterating a large wall gap.
                const double period = definition.loopEnd - definition.loopBegin;
                if (settled && !state.stopAfterBlend && dt >= period) {
                    const double cycles = std::floor(dt / period);
                    const double remainder = std::fmod(dt, period);
                    state.blend.advance(dt - remainder); dt = remainder;
                    const auto available = std::numeric_limits<std::uint64_t>::max() - counters_.wraps;
                    counters_.wraps += cycles < static_cast<double>(available) ? static_cast<std::uint64_t>(cycles) : available;
                }
            }
            else { cross(i, end); state.playing = false; break; }
        } else if (state.time >= next) cross(i, state.time);
        else break;
    }
}
void Runtime::evaluate() {
    for (auto& value : values_) value.second = 0; // Retain map nodes across steady playback.
    for (const auto& [label, animator] : variables_) values_[label] = animator.value;
    for (std::size_t i = 0; i < states_.size(); ++i) {
        const auto& state = states_[i]; if (!state.playing || state.difference) continue;
        for (std::size_t k = 0; k < state.tracks.size(); ++k) {
            if (state.cursors[k] < 0) continue;
            const auto& label = definitions_[i].tracks[k].label;
            const double base = values_[label];
            values_[label] = base + (state.tracks[k].value - base) * state.blend.value;
        }
    }
    for (std::size_t i = 0; i < states_.size(); ++i) {
        const auto& state = states_[i]; if (!state.playing || !state.difference) continue;
        for (std::size_t k = 0; k < state.tracks.size(); ++k) if (state.cursors[k] >= 0)
            values_[definitions_[i].tracks[k].label] += state.tracks[k].value * state.blend.value;
    }
}
void Runtime::advance(double dt) {
    if (!finite(dt) || dt < 0) return;
    ++counters_.steps;
    bool controllersActive = false;
    for (const auto& entry : variables_) controllersActive |= entry.second.active();
    double remaining = dt;
    while (remaining > 0) {
        double step = remaining;
        // Synchronize base controllers with crossings when their values change.
        // Stable bases can use the O(number of authored keys) cycle fast path.
        bool needsBoundary = controllersActive;
        for (std::size_t i = 0; i < states_.size(); ++i)
            if (states_[i].playing && !(definitions_[i].loopBegin >= 0 && definitions_[i].loopEnd > definitions_[i].loopBegin)) needsBoundary = true;
        if (needsBoundary)
            for (std::size_t i = 0; i < states_.size(); ++i) if (states_[i].playing) {
                const auto& state = states_[i]; const auto& definition = definitions_[i];
                const double end = definition.loopBegin >= 0 && definition.loopEnd > definition.loopBegin ? definition.loopEnd : std::max(0.0, definition.lastTime);
                if (end > state.time) step = std::min(step, end - state.time);
                for (std::size_t k = 0; k < definition.tracks.size(); ++k) {
                    const auto n = state.cursors[k] + 1;
                    if (n < static_cast<int>(definition.tracks[k].frames.size()) && definition.tracks[k].frames[n].time > state.time)
                        step = std::min(step, definition.tracks[k].frames[n].time - state.time);
                }
            }
        for (auto& [label, animator] : variables_) animator.advance(step);
        for (std::size_t i = 0; i < states_.size(); ++i) {
            const bool wasPlaying = states_[i].playing; advanceState(i, step);
            // A completed main timeline retains its final pose; stopped diff contributes zero.
            if (wasPlaying && !states_[i].playing && !states_[i].difference && !states_[i].stopAfterBlend)
                for (std::size_t k = 0; k < states_[i].tracks.size(); ++k)
                    if (states_[i].cursors[k] >= 0) variables_[definitions_[i].tracks[k].label].assign(states_[i].tracks[k].value);
        }
        remaining = std::max(0.0, remaining - step);
        controllersActive = false;
        for (const auto& entry : variables_) controllersActive |= entry.second.active();
    }
    evaluate();
}
void Runtime::seek(double time) {
    if (!finite(time) || time < 0) return;
    for (std::size_t i = 0; i < states_.size(); ++i) if (states_[i].playing) {
        const auto& d = definitions_[i]; double t = time;
        if (d.loopBegin >= 0 && d.loopEnd > d.loopBegin && t >= d.loopEnd)
            t = d.loopBegin + std::fmod(t - d.loopBegin, d.loopEnd - d.loopBegin);
        else if (d.lastTime >= 0) t = std::min(t, d.lastTime);
        initialize(i, 0); advanceState(i, t);
    }
    evaluate();
}
namespace {
void writeAnimator(std::ostream& out, const Animator& a) {
    out << a.value << ' ' << a.start << ' ' << a.target << ' ' << a.duration << ' ' << a.elapsed << ' ' << a.weight << ' ' << a.queue.size() << '\n';
    for (const auto& r : a.queue) out << r.target << ' ' << r.duration << ' ' << r.weight << '\n';
}
bool readAnimator(std::istream& in, Animator& a) {
    std::size_t count;
    if (!(in >> a.value >> a.start >> a.target >> a.duration >> a.elapsed >> a.weight >> count) || count > 65536 ||
        !finite(a.value) || !finite(a.start) || !finite(a.target) || !finite(a.duration) || !finite(a.elapsed) || !finite(a.weight) ||
        a.duration < 0 || a.elapsed < 0 || a.elapsed > a.duration || a.weight <= 0) return false;
    a.queue.clear();
    for (std::size_t i = 0; i < count; ++i) {
        Request r; if (!(in >> r.target >> r.duration >> r.weight) || !finite(r.target) || !finite(r.duration) || !finite(r.weight) || r.duration <= 0 || r.weight <= 0) return false;
        a.queue.push_back(r);
    }
    return true;
}
}
std::string Runtime::serialize() const {
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(17);
    out << "EMOTE1 " << variables_.size() << ' ' << states_.size() << ' ' << definitionFingerprint() << ' ' << queuing_ << '\n';
    for (const auto& [label, a] : variables_) { out << std::quoted(encodeLabel(label)) << '\n'; writeAnimator(out, a); }
    for (const auto& s : states_) {
        out << std::quoted(encodeLabel(s.label)) << ' ' << s.time << ' ' << s.flags << ' ' << s.playing << ' ' << s.difference << ' ' << s.stopAfterBlend << ' ' << s.tracks.size() << '\n';
        writeAnimator(out, s.blend);
        for (std::size_t k = 0; k < s.tracks.size(); ++k) { out << s.cursors[k] << '\n'; writeAnimator(out, s.tracks[k]); }
    }
    return out.str();
}
bool Runtime::restore(const std::string& text) {
    if (text.size() > 16 * 1024 * 1024) return false;
    Runtime candidate = *this; std::istringstream in(text); in.imbue(std::locale::classic());
    std::string magic; std::size_t variables, states; std::uint64_t fingerprint;
    if (!(in >> magic >> variables >> states >> fingerprint >> candidate.queuing_) || magic != "EMOTE1" || variables > 65536 || states != states_.size() || fingerprint != definitionFingerprint()) return false;
    candidate.variables_.clear();
    for (std::size_t i = 0; i < variables; ++i) {
        std::string label; Animator a;
        if (!(in >> std::quoted(label)) || !decodeLabel(label) || !readAnimator(in, a) || !candidate.variables_.emplace(label, a).second) return false;
    }
    for (std::size_t i = 0; i < states; ++i) {
        auto& s = candidate.states_[i]; std::string label; std::size_t tracks;
        if (!(in >> std::quoted(label) >> s.time >> s.flags >> s.playing >> s.difference >> s.stopAfterBlend >> tracks) ||
            !decodeLabel(label) || label != s.label || !finite(s.time) || s.time < 0 || tracks != s.tracks.size() || !readAnimator(in, s.blend)) return false;
        const auto& definition = definitions_[i];
        const double end = definition.loopBegin >= 0 && definition.loopEnd > definition.loopBegin ? definition.loopEnd : std::max(0.0, definition.lastTime);
        if (s.time > end || s.blend.value < 0 || s.blend.value > 1) return false;
        for (std::size_t k = 0; k < tracks; ++k) {
            if (!(in >> s.cursors[k]) || s.cursors[k] < -1 || s.cursors[k] >= static_cast<int>(definitions_[i].tracks[k].frames.size()) || !readAnimator(in, s.tracks[k])) return false;
        }
    }
    in >> std::ws; if (!in.eof()) return false;
    candidate.evaluate(); *this = std::move(candidate); return true;
}
std::uint64_t Runtime::definitionFingerprint() const {
    std::uint64_t hash = 14695981039346656037ULL;
    auto integer = [&](std::uint64_t bits) { for (int i = 0; i < 8; ++i) { hash ^= (bits >> (i * 8)) & 255; hash *= 1099511628211ULL; } };
    auto number = [&](double value) { std::uint64_t bits; std::memcpy(&bits, &value, sizeof(bits)); integer(bits); };
    auto label = [&](const std::string& text) { integer(text.size()); for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ULL; } };
    integer(definitions_.size());
    for (const auto& definition : definitions_) {
        label(definition.label); number(definition.loopBegin); number(definition.loopEnd); number(definition.lastTime); integer(definition.difference); integer(definition.tracks.size());
        for (const auto& track : definition.tracks) {
            label(track.label); integer(track.instant); integer(track.frames.size());
            for (const auto& frame : track.frames) { number(frame.time); number(frame.value); number(frame.easing); integer(frame.content); }
        }
    }
    return hash;
}
} // namespace emoteplayer::animation
