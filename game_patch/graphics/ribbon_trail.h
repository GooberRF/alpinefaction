#pragma once

#include <algorithm>
#include <array>
#include "../rf/gr/gr.h"
#include "../rf/math/vector.h"

// A fading trail of `Lines` polylines sampled together, such as one per foot or per wingtip: a ring of the last
// `Capacity` samples, drawn as camera-facing additive ribbons that clear with age.
template<int Lines, int Capacity>
struct RibbonTrail
{
    struct Sample
    {
        std::array<rf::Vector3, Lines> points{};
        float age = 0.0f;
        float strength = 1.0f; // scales the sample's alpha
        bool joins_previous = false;
    };

    std::array<Sample, Capacity> samples{};
    int first = 0; // the oldest sample's slot
    int count = 0;
    bool live = false; // the newest sample joins the source's current position

    Sample& sample(int i)
    {
        return samples[(first + i) % Capacity];
    }

    const Sample& sample(int i) const
    {
        return samples[(first + i) % Capacity];
    }

    void reset()
    {
        count = 0;
        live = false;
    }

    void push(const std::array<rf::Vector3, Lines>& points, float strength = 1.0f)
    {
        if (count == Capacity) {
            first = (first + 1) % Capacity;
            --count;
        }
        Sample& s = sample(count++);
        s.points = points;
        s.age = 0.0f;
        s.strength = strength;
        s.joins_previous = live;
        live = true;
    }

    void age(float dt, float lifetime)
    {
        for (int i = 0; i < count; ++i) {
            sample(i).age += dt;
        }
        while (count > 0 && sample(0).age >= lifetime) {
            first = (first + 1) % Capacity;
            --count;
        }
    }
};

struct RibbonTrailStyle
{
    float lifetime;
    float alpha;
    float band; // opaque share of the ribbon's half-width
    float min_half_width;
    float min_half_px;
};

// Whether a trail at `anchor` is drawn in the current view, with its fog fade and half-width. Ribbons are not fogged,
// so they are faded by hand and dropped past the far plane, where they would depth-test against the cleared sky.
bool ribbon_trail_view(const rf::Vector3& anchor, const RibbonTrailStyle& style, float& fade, float& half_width);
// A camera-facing ribbon along the first n points, each at its alpha. Needs the texture cleared.
void ribbon_trail_stroke(int n, const rf::Vector3* pos, const float* alpha, float half_width, float band,
                         const rf::gr::Color& color);

// Draws each line of `trail`, eased out with age and clear at each stroke's tail. `head`, when given, extends the
// newest sample to the source's current points at `head_strength`.
template<int Lines, int Capacity>
void ribbon_trail_render(const RibbonTrail<Lines, Capacity>& trail,
                         const std::array<rf::Vector3, static_cast<std::size_t>(Lines)>* head,
                         float head_strength, const RibbonTrailStyle& style, float fade, float half_width,
                         const rf::gr::Color& color)
{
    static std::array<rf::Vector3, Capacity + 1> stroke_pos;
    static std::array<float, Capacity + 1> stroke_alpha;
    for (int line = 0; line < Lines; ++line) {
        int n = 0;
        for (int i = 0; i < trail.count; ++i) {
            const auto& s = trail.sample(i);
            if (!s.joins_previous && n > 1) {
                ribbon_trail_stroke(n, stroke_pos.data(), stroke_alpha.data(), half_width, style.band, color);
            }
            if (!s.joins_previous) {
                n = 0;
            }
            const float t = 1.0f - std::min(s.age / style.lifetime, 1.0f);
            stroke_pos[n] = s.points[line];
            stroke_alpha[n] = n == 0 ? 0.0f : style.alpha * s.strength * fade * t * t;
            ++n;
        }
        if (head && trail.live) {
            stroke_pos[n] = (*head)[line];
            stroke_alpha[n] = n == 0 ? 0.0f : style.alpha * head_strength * fade;
            ++n;
        }
        if (n > 1) {
            ribbon_trail_stroke(n, stroke_pos.data(), stroke_alpha.data(), half_width, style.band, color);
        }
    }
}
