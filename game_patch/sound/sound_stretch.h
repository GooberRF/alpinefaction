#pragma once

#include <vector>

// Time-stretches interleaved PCM samples (-1 to 1) by WSOLA (waveform similarity overlap-add): the result plays speed
// times as fast at the same pitch. Meant for speeds of 0.5 to 2. A circular input is a loop and the result loops
// seamlessly too, its length rounded to whole synthesis hops. Empty when the input is too short to stretch.
std::vector<float> wsola_stretch(const std::vector<float>& samples, int num_channels, int sample_rate, float speed,
                                 bool circular);
