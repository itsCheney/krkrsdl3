// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mikage contributors.
// Adapted animation behavior from AetherKiri/AetherKrkr fa0f8af; see AETHER-NOTICE.md.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace emoteplayer::animation {
// Labels retain the production PSB reader's trailing-NUL convention.
struct Frame { double time = 0, value = 0, easing = 0; bool content = false; };
struct Track { std::string label; std::vector<Frame> frames; bool instant = false; };
struct Timeline {
    std::string label;
    double loopBegin = -1, loopEnd = -1, lastTime = -1;
    bool difference = false;
    std::vector<Track> tracks;
};
struct Request { double target = 0, duration = 0, weight = 1; };
struct Animator {
    double value = 0, start = 0, target = 0, duration = 0, elapsed = 0, weight = 1;
    std::deque<Request> queue;
    void assign(double v);
    void schedule(double v, double time, double easing, bool queuing = false);
    void advance(double dt);
    bool active() const { return elapsed < duration || !queue.empty(); }
};
struct TimelineState {
    std::string label;
    double time = 0;
    int flags = 0;
    bool playing = false, difference = false, stopAfterBlend = false;
    Animator blend;
    std::vector<Animator> tracks;
    std::vector<int> cursors;
};
struct Counters { std::uint64_t steps = 0, wraps = 0, crossings = 0; };

// Resource definitions are copied once; all mutable state belongs to this object.
// A copy is a complete independent clone, including unfinished requests.
class Runtime {
public:
    void reset(std::vector<Timeline> definitions, const std::map<std::string, double>& defaults);
    bool play(const std::string& label, int flags);
    void stop(const std::string& label = {});
    bool playing(const std::string& label = {}) const;
    bool active() const;
    void setVariable(const std::string& label, double value, double time = 0, double easing = 0);
    double variable(const std::string& label) const;
    bool hasVariable(const std::string& label) const;
    void setBlend(const std::string& label, double ratio, double time, double easing, bool autoStop = false);
    void fadeIn(const std::string& label, double time, double easing);
    void fadeOut(const std::string& label, double time, double easing);
    void advance(double dt);
    void seek(double time);
    void setQueuing(bool value) { queuing_ = value; }
    bool queuing() const { return queuing_; }
    const Timeline* definition(const std::string& label) const;
    const TimelineState* state(const std::string& label) const;
    const std::vector<TimelineState>& states() const { return states_; }
    const std::map<std::string, double>& values() const { return values_; }
    const Counters& counters() const { return counters_; }
    std::string serialize() const;
    bool restore(const std::string& text); // Transactional; malformed snapshots leave state intact.
private:
    std::vector<Timeline> definitions_;
    std::vector<TimelineState> states_;
    std::map<std::string, Animator> variables_;
    std::map<std::string, double> values_;
    Counters counters_;
    bool queuing_ = false;
    std::size_t index(const std::string& label) const;
    void initialize(std::size_t i, double time, bool preserveValue = false);
    void cross(std::size_t i, double time);
    void advanceState(std::size_t i, double dt);
    void evaluate();
    std::uint64_t definitionFingerprint() const;
};
double easeWeight(double easing);
double millisecondsToFrames(double ms, double speedDivisor);
} // namespace emoteplayer::animation
