#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace emoteplayer
{
inline float emoteParameterTick(float value, float begin, float end, float division)
{
    if (!std::isfinite(value) || begin == end) return 0;
    // Apply the domain of each motion after pose, timeline and physics have
    // been composed. Additive motion may exceed an authored endpoint; a
    // negative tick would skip that node's transform and detach its children.
    return division * std::clamp((value - begin) / (end - begin), 0.0f, 1.0f);
}

// E-mote control times and the physics coefficients are expressed in 60 Hz frames.
inline double emoteEase(double t, double easing)
{
    t = std::clamp(t, 0.0, 1.0);
    const double exponent = easing >= 0 ? 1 + easing : 1 / (1 - easing);
    return std::pow(t, exponent);
}

struct EmoteTween
{
    double from = 0, to = 0, start = 0, duration = 0, easing = 0;
    double value(double tick) const
    {
        return duration > 0 ? from + (to - from) * emoteEase((tick - start) / duration, easing) : to;
    }
    void set(double current, double target, double tick, double time = 0, double ease = 0)
    {
        from = current; to = target; start = tick;
        duration = std::isfinite(time) ? std::max(0.0, time) : 0;
        easing = ease;
    }
    bool done(double tick) const { return tick >= start + duration; }
};

struct Vector3
{
    double x = 0, y = 0, z = 0;
    Vector3() = default;
    Vector3(double x, double y, double z = 0) : x(x), y(y), z(z) {}
    Vector3 operator+(const Vector3& b) const { return {x + b.x, y + b.y, z + b.z}; }
    Vector3 operator-(const Vector3& b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vector3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vector3 operator/(double s) const { return {x / s, y / s, z / s}; }
    double magnitude() const { return std::sqrt(x * x + y * y + z * z); }
    Vector3 normalized() const { const auto m = magnitude(); return m > 0 ? *this / m : *this; }
    static Vector3 zero() { return {}; }
};

class emotefile;
class bustControl
{
public:
    bustControl() = default;
    bustControl(emotefile* file, unsigned int offset);
    ~bustControl();
    struct BustState
    {
        Vector3 op, p, pv;
        double ofs = 0;
    } param;
    bool enabled = true;
    std::string baseLayer, label;
    double spring = 0.03125, friction = 0.125, gravity = 0.3;
    double scale_x = 1, scale_y = 1;
    std::string var_lr, var_ud;
};

struct uniSegment
{
    Vector3 bp, p, pv;
    double scale_x = 1, scale_y = 1, length = 0;
};
class uniControl
{
public:
    uniControl() = default;
    uniControl(emotefile* file, unsigned int offset);
    ~uniControl();
    bool enabled = true;
    std::string baseLayer, label;
    std::vector<uniSegment> segments;
    double b_rate = 0.00390625, gravity = 0.5;
    double friction_x = 0.03125, friction_y = 0.09375;
    double bend_spd = 0, bend_vol = 0, bendR = 0, bendS = 0, ofs = 0;
    Vector3 op;
    double ud_eft = 0, v_bound = 1;
    std::string var_lr, var_lrm, var_ud;
};

// Resource controls above are immutable templates. Integrators and anchors belong
// to one player, including when multiple players share the same cached PSB.
class EmotePhysics
{
    struct Anchor
    {
        Vector3 previous, current, origin;
        bool initialized = false, available = false;
        void set(Vector3 value) { current = value; available = true; }
    };
    struct Bust
    {
        bustControl rule;
        Anchor anchor;
        Vector3 p, velocity;
    };
    struct Chain
    {
        uniControl rule;
        Anchor anchor;
        std::vector<Vector3> p, velocity;
        double phase = 0, bend = 0;
        bool hair = true;
    };
    std::vector<Bust> busts;
    std::vector<Chain> chains;
    double remainder = 0;
    bool wind = false;
    double windStart = 0, windGoal = 0, windSpeed = 0, windMin = 0, windMax = 0;
    struct Gust { double position = 0, strength = 0; bool active = false; };
    std::array<Gust, 128> gusts{};
    double windRemainder = 0;
    uint32_t windRandom = 0x9e3779b9;

    double randomWind()
    {
        windRandom ^= windRandom << 13;
        windRandom ^= windRandom >> 17;
        windRandom ^= windRandom << 5;
        return (windRandom & 0x7fffff) / 8388608.0;
    }
    void advanceWind()
    {
        if (!wind) return;
        windRemainder += std::abs(windSpeed);
        while (windRemainder >= 1)
        {
            windRemainder -= 1;
            if (randomWind() >= 0.0625) continue;
            for (auto& gust : gusts)
                if (!gust.active)
                {
                    gust = {windStart, windMin + (windMax - windMin) * randomWind(), true};
                    break;
                }
        }
        for (auto& gust : gusts)
            if (gust.active)
            {
                gust.position += windSpeed;
                if (windSpeed > 0 ? gust.position >= windGoal : gust.position <= windGoal) gust.active = false;
            }
    }

    static double response(double displacement)
    {
        // Authored +/-30 pose channels need a smooth limit, including during
        // abrupt placement changes. The gain belongs inside this curve.
        constexpr double angle = 0.039269909262657166;
        return std::atan(displacement * 1.149999976158142 * angle) / angle;
    }
    static void integrate(Vector3& p, Vector3& velocity, Vector3 force,
                          double fx, double fy, double dt)
    {
        velocity = velocity + force * dt;
        velocity.x *= std::max(0.0, 1 - fx * dt);
        velocity.y *= std::max(0.0, 1 - fy * dt);
        p = p + velocity * dt;
    }
    double windAt(Vector3 p) const
    {
        if (!wind || windStart == windGoal) return 0;
        // Wind consists of sparse travelling gusts, rather than a permanent
        // force applied to the whole range (which leaves hair fully deflected).
        for (const auto& gust : gusts)
            if (gust.active && std::abs(p.x - gust.position) < std::abs(gust.strength * 2))
                return gust.strength * windSpeed;
        return 0;
    }

public:
    double bustScale = 1, hairScale = 1, partsScale = 1;
    double rotation = 0;
    Vector3 bustForce, hairForce, partsForce;
    Vector3 localForce(Vector3 force) const
    {
        const double cosine = std::cos(rotation), sine = std::sin(rotation);
        return {cosine * force.x + sine * force.y, -sine * force.x + cosine * force.y};
    }
    // Bumped whenever the set of anchor layers may change.
    std::uint64_t anchorGeneration = 0;
    void reset(const std::vector<bustControl*>& bust, const std::vector<uniControl*>& hair,
               const std::vector<uniControl*>& parts)
    {
        ++anchorGeneration;
        busts.clear(); chains.clear(); remainder = 0;
        stopWind(); windRandom = 0x9e3779b9;
        for (auto* rule : bust)
            if (rule && rule->enabled) { Bust b; b.rule = *rule; busts.push_back(std::move(b)); }
        auto add = [&](const std::vector<uniControl*>& list, bool isHair)
        {
            for (auto* rule : list)
                if (rule && rule->enabled && !rule->segments.empty())
                {
                    Chain c; c.rule = *rule; c.hair = isHair; c.phase = rule->bendR; c.bend = rule->bendS;
                    c.p.resize(rule->segments.size()); c.velocity.resize(c.p.size());
                    chains.push_back(std::move(c));
                }
        };
        add(hair, true); add(parts, false);
    }
    bool empty() const { return busts.empty() && chains.empty(); }
    bool needsAnchor(const std::string& label) const
    {
        for (const auto& b : busts) if (b.rule.baseLayer == label) return true;
        for (const auto& c : chains) if (c.rule.baseLayer == label) return true;
        return false;
    }
    void beginAnchors()
    {
        for (auto& b : busts) b.anchor.available = false;
        for (auto& c : chains) c.anchor.available = false;
    }
    void setAnchor(const std::string& label, Vector3 p)
    {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return;
        for (auto& b : busts) if (b.rule.baseLayer == label) b.anchor.set(p);
        for (auto& c : chains) if (c.rule.baseLayer == label) c.anchor.set(p);
    }
    void startWind(double start, double goal, double speed, double minimum, double maximum)
    {
        if (!std::isfinite(start) || !std::isfinite(goal) || !std::isfinite(speed) ||
            !std::isfinite(minimum) || !std::isfinite(maximum)) return;
        wind = true; windStart = start; windGoal = goal;
        windSpeed = (goal > start ? 1 : -1) * std::min(std::abs(speed), 128.0);
        gusts = {}; windRemainder = 0;
        windMin = std::min(minimum, maximum); windMax = std::max(minimum, maximum);
    }
    void stopWind() { wind = false; gusts = {}; windRemainder = 0; }

    template<class Write> void update(double frames, Write write)
    {
        if (!std::isfinite(frames) || frames < 0) return;
        // Coefficients were authored for a 60 Hz Euler step. Accumulating fixed
        // steps preserves their damping at both low and high render rates.
        // Bound suspended intervals so resuming never runs an unbounded backlog.
        const double elapsed = std::min(frames, 15.0);
        remainder += elapsed;
        const int steps = (int)std::floor(remainder + 1e-9);
        remainder -= steps;
        for (auto& b : busts)
            if (b.anchor.available && !b.anchor.initialized)
            {
                b.anchor.origin = b.anchor.previous = b.anchor.current; b.anchor.initialized = true;
                b.p = b.rule.param.p;
                b.velocity = b.rule.param.pv;
            }
        for (auto& c : chains)
            if (c.anchor.available && !c.anchor.initialized)
            {
                c.anchor.origin = c.anchor.previous = c.anchor.current; c.anchor.initialized = true;
                for (size_t i = 0; i < c.p.size(); ++i)
                {
                    c.p[i] = c.rule.segments[i].p;
                    c.velocity[i] = c.rule.segments[i].pv;
                }
            }
        for (int step = 0; step < steps; ++step)
        {
            const double fraction = (step + 1.0) / steps;
            advanceWind();
            for (auto& b : busts)
            {
                if (!b.anchor.available) continue;
                const auto anchor = b.rule.param.op + b.anchor.previous - b.anchor.origin +
                                    (b.anchor.current - b.anchor.previous) * fraction;
                const auto force = localForce(bustForce + Vector3(0, b.rule.gravity)) +
                                   (anchor - b.p) * b.rule.spring;
                integrate(b.p, b.velocity, force, b.rule.friction, b.rule.friction, 1);
            }
            for (auto& c : chains)
            {
                if (!c.anchor.available) continue;
                const auto anchor = c.rule.op + c.anchor.previous - c.anchor.origin +
                                    (c.anchor.current - c.anchor.previous) * fraction;
                const auto external = localForce((c.hair ? hairForce : partsForce) + Vector3(0, c.rule.gravity));
                for (size_t i = 0; i < c.p.size(); ++i)
                {
                    const auto& segment = c.rule.segments[i];
                    const auto parent = i ? c.p[i - 1] : anchor;
                    auto direction = parent - c.p[i];
                    const double distance = direction.magnitude();
                    if (distance > segment.length && distance > 1e-9)
                    {
                        direction = direction / distance;
                        const double extension = distance - segment.length;
                        if (!i) c.velocity[i] = c.velocity[i] + direction * (extension * c.rule.b_rate);
                        else
                        {
                            c.p[i] = c.p[i] + direction * extension;
                            const auto& v = c.velocity[i];
                            const double along = v.x * direction.x + v.y * direction.y + v.z * direction.z;
                            c.velocity[i] = v + direction * (-along * c.rule.v_bound);
                        }
                    }
                    integrate(c.p[i], c.velocity[i], external + Vector3(windAt(c.p[i]), 0),
                              c.rule.friction_x, c.rule.friction_y, 1);
                }
                const double gain = c.hair ? hairScale : partsScale;
                const double primary = response((c.p.front().x - anchor.x) * c.rule.segments.front().scale_x * gain);
                // Extra bending is excited by a strong swing; it must decay at
                // rest instead of driving an artificial perpetual oscillation.
                c.bend = std::clamp(c.bend + (std::abs(primary) > 28 ? 1 : -1) / 32.0, 0.0, 1.0);
                c.phase = std::fmod(c.phase + c.rule.bend_spd * c.bend, 6.283185307179586);
            }
        }
        for (auto& b : busts)
        {
            if (!b.anchor.available) continue;
            const auto anchor = b.rule.param.op + b.anchor.current - b.anchor.origin;
            const auto offset = (b.p - anchor) * bustScale;
            write(b.rule.var_lr, response(offset.x * b.rule.scale_x));
            write(b.rule.var_ud, response((offset.y - b.rule.param.ofs) * b.rule.scale_y));
            if (steps) b.anchor.previous = b.anchor.current;
        }
        for (auto& c : chains)
        {
            if (!c.anchor.available) continue;
            const double scale = c.hair ? hairScale : partsScale;
            const auto anchor = c.rule.op + c.anchor.current - c.anchor.origin;
            const double wave = std::sin(c.phase) * c.bend * c.rule.bend_vol;
            write(c.rule.var_lrm, response((c.p.front().x - anchor.x) * c.rule.segments.front().scale_x * scale) + wave);
            write(c.rule.var_lr, response((c.p.back().x - anchor.x) * c.rule.segments.back().scale_x * scale) - wave);
            const size_t vertical = size_t(std::clamp(c.rule.ud_eft, 0.0, double(c.p.size() - 1)));
            double restY = anchor.y;
            for (size_t i = 0; i <= vertical; ++i) restY += c.rule.segments[i].length;
            write(c.rule.var_ud, response((c.p[vertical].y - restY + c.rule.ofs) *
                                         c.rule.segments[vertical].scale_y * scale));
            if (steps) c.anchor.previous = c.anchor.current;
        }
    }
};
} // namespace emoteplayer
