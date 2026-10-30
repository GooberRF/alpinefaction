#include <algorithm>
#include <cmath>
#include <numbers>
#include "sound_stretch.h"

namespace
{
    // The input as WSOLA reads it: a linear input is padded with silence (and starts after a hop of it, so the first
    // window's fade-in falls on silence and the sound's attack is kept), a circular one repeats
    struct WsolaInput
    {
        const std::vector<float>& samples;
        std::vector<float> mono;
        int num_channels;
        int num_frames;
        int lead_in;
        bool circular;

        // Frame index into samples, -1 for padding
        int frame(int i) const
        {
            if (circular) {
                i %= num_frames;
                return i < 0 ? i + num_frames : i;
            }
            i -= lead_in;
            return i >= 0 && i < num_frames ? i : -1;
        }

        float mono_at(int i) const
        {
            const int f = frame(i);
            return f < 0 ? 0.0f : mono[f];
        }

        float similarity(int a, int b, int len, int step) const
        {
            float sum = 0.0f;
            for (int j = 0; j < len; j += step) {
                sum += mono_at(a + j) * mono_at(b + j);
            }
            return sum;
        }
    };
}

std::vector<float> wsola_stretch(const std::vector<float>& samples, int num_channels, int sample_rate, float speed,
                                 bool circular)
{
    // 25 ms windows at 50% overlap (periodic Hann windows sum to 1), aligned within 8 ms of their nominal position.
    // The search runs at about 5.5 kHz first, then refines at full rate around the best match.
    const int window_len = std::max(64, sample_rate / 40) & ~1;
    const int hop = window_len / 2;
    const int tolerance = sample_rate / 125;
    const int coarse_step = std::max(1, sample_rate / 5512);
    const int num_frames = static_cast<int>(samples.size()) / num_channels;
    if (num_channels < 1 || num_frames < 2 * window_len || speed <= 0.0f) {
        return {};
    }

    WsolaInput input{samples, std::vector<float>(num_frames), num_channels, num_frames, circular ? 0 : hop, circular};
    for (int i = 0; i < num_frames; ++i) {
        float sum = 0.0f;
        for (int c = 0; c < num_channels; ++c) {
            sum += samples[i * num_channels + c];
        }
        input.mono[i] = sum;
    }

    int out_frames = 0;
    int num_windows = 0;
    double analysis_hop = 0.0;
    if (circular) {
        num_windows = std::max(2, static_cast<int>(std::lround(num_frames / speed / hop)));
        out_frames = num_windows * hop;
        analysis_hop = static_cast<double>(num_frames) / num_windows;
    }
    else {
        out_frames = static_cast<int>(std::lround(num_frames / speed));
        num_windows = (out_frames + hop - 1) / hop + 1;
        analysis_hop = hop * static_cast<double>(speed);
    }

    std::vector<float> window(window_len);
    for (int j = 0; j < window_len; ++j) {
        window[j] = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * j / window_len);
    }

    std::vector<float> out(static_cast<size_t>(circular ? out_frames : (num_windows + 1) * hop) * num_channels);
    int prev_pos = 0;
    for (int k = 0; k < num_windows; ++k) {
        int pos = 0;
        if (k > 0) {
            // The window that best continues the previous one (what follows it in the input), near its nominal
            // position; the last window of a loop must also lead into the first
            const int continuation = prev_pos + hop;
            const bool wraps = circular && k == num_windows - 1;
            const auto score = [&](int p, int step) {
                float s = input.similarity(p, continuation, window_len, step);
                if (wraps) {
                    s += input.similarity(p + hop, 0, hop, step);
                }
                return s;
            };
            const int nominal = static_cast<int>(std::lround(k * analysis_hop));
            const int lo = std::max(0, nominal - tolerance);
            const int hi = nominal + tolerance;
            float best_score = -INFINITY;
            for (int p = lo; p <= hi; p += coarse_step) {
                const float s = score(p, coarse_step);
                if (s > best_score) {
                    best_score = s;
                    pos = p;
                }
            }
            const int coarse_pos = pos;
            best_score = -INFINITY;
            for (int p = std::max(lo, coarse_pos - coarse_step + 1); p <= std::min(hi, coarse_pos + coarse_step - 1); ++p) {
                const float s = score(p, 1);
                if (s > best_score) {
                    best_score = s;
                    pos = p;
                }
            }
        }
        for (int j = 0; j < window_len; ++j) {
            const int f = input.frame(pos + j);
            if (f < 0) {
                continue;
            }
            int o = k * hop + j;
            if (circular) {
                o %= out_frames;
            }
            for (int c = 0; c < num_channels; ++c) {
                out[o * num_channels + c] += window[j] * samples[f * num_channels + c];
            }
        }
        prev_pos = pos;
    }

    if (!circular) {
        // Drop the lead-in
        out.erase(out.begin(), out.begin() + static_cast<ptrdiff_t>(hop) * num_channels);
        out.resize(static_cast<size_t>(out_frames) * num_channels);
    }
    return out;
}
