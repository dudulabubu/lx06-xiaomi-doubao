#include "audio/kws_preprocess.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace xiaoai_plus::audio {

namespace {
// 31-tap symmetric Kaiser-windowed sinc LPF (beta=6.0, fc=7000 Hz @ fs=48000 Hz).
// Passband flat to 5.5 kHz, stopband -56 dB @ 10 kHz / -82 dB @ 12 kHz, exact unity DC gain.
constexpr std::array<float, 16> kFirHalf = {
    0.00029172f, 0.00022267f, -0.00107668f, -0.00316846f,
    -0.00317253f, 0.00209102f, 0.01108221f, 0.01494100f,
    0.00316705f, -0.02381165f, -0.04621450f, -0.03265999f,
    0.03637192f, 0.14646270f, 0.24957953f, 0.29178795f,
};

inline int16_t FloatToS16(float v) {
  const float clamped = std::max(-32768.0f, std::min(32767.0f, v));
  return static_cast<int16_t>(std::nearbyintf(clamped));
}
}  // namespace

void ConvertKwsS32ToS16(const int32_t* input, size_t samples, int shift,
                        int16_t* output) {
  if (shift < 8 || shift > 16) {
    throw std::invalid_argument("KWS S32 shift must be in 8..16");
  }
  if (!input || !output) return;
  for (size_t i = 0; i < samples; ++i) {
    const int32_t scaled = input[i] >> shift;
    output[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, scaled)));
  }
}

Lx06Decimator48To16::Lx06Decimator48To16() { Reset(); }

void Lx06Decimator48To16::Reset() {
  for (auto& h : hist_) h.fill(0.0f);
  hp_x_prev_.fill(0.0f);
  hp_y_prev_.fill(0.0f);
}

void Lx06Decimator48To16::Process(const int32_t* input, size_t in_frames,
                                  int mic_shift, std::vector<uint8_t>* output) {
  if (!output) return;
  if (!input || in_frames < kDecimFactor) {
    output->clear();
    return;
  }
  const int safe_shift = std::max(8, std::min(16, mic_shift));
  const float mic_scale = 1.0f / static_cast<float>(1u << safe_shift);
  constexpr float kRefScale = 1.0f / 65536.0f;  // ch7 upper 16 bits (Xiaomi AudioRecord_Read)
  constexpr float kHpR = 0.980f;                // ~51 Hz DC-blocking high-pass @ 16 kHz
  constexpr float kHpGain = 0.5f * (1.0f + kHpR);

  const size_t out_frames = in_frames / kDecimFactor;
  const size_t used_in = out_frames * kDecimFactor;
  output->resize(out_frames * kChannels * sizeof(int16_t));
  auto* dst = reinterpret_cast<int16_t*>(output->data());

  // Zero unused ch6 once per output frame; active channels fill 0..5 and 7.
  for (size_t o = 0; o < out_frames; ++o) {
    dst[o * kChannels + 6] = 0;
  }

  work_.resize(kHist + used_in);

  // Process 7 active channels: slots 0..5 -> raw ch0..ch5 (mics), slot 6 -> raw ch7 (loopback ref).
  for (size_t slot = 0; slot < 7; ++slot) {
    const size_t raw_ch = (slot < 6) ? slot : 7;
    const float scale = (slot < 6) ? mic_scale : kRefScale;

    auto& h = hist_[slot];
    std::copy(h.begin(), h.end(), work_.begin());
    for (size_t i = 0; i < used_in; ++i) {
      work_[kHist + i] = static_cast<float>(input[i * kChannels + raw_ch]) * scale;
    }
    std::copy(work_.begin() + static_cast<long>(used_in),
              work_.begin() + static_cast<long>(used_in + kHist), h.begin());

    float xp = (slot < 6) ? hp_x_prev_[slot] : 0.0f;
    float yp = (slot < 6) ? hp_y_prev_[slot] : 0.0f;

    for (size_t o = 0; o < out_frames; ++o) {
      const float* w = work_.data() + o * kDecimFactor;
      float acc = kFirHalf[15] * w[15];
      for (size_t k = 0; k < 15; ++k) {
        acc += kFirHalf[k] * (w[k] + w[30 - k]);
      }
      if (slot < 6) {
        const float y = kHpGain * (acc - xp) + kHpR * yp;
        xp = acc;
        yp = y;
        dst[o * kChannels + raw_ch] = FloatToS16(y);
      } else {
        dst[o * kChannels + raw_ch] = FloatToS16(acc);
      }
    }

    if (slot < 6) {
      hp_x_prev_[slot] = xp;
      hp_y_prev_[slot] = yp;
    }
  }
}

void KwsAgc::Process(int16_t* samples, size_t count) {
  if (!samples) return;
  constexpr double kTargetRms = 0.080;     // -22 dBFS target for Zipformer fbank
  constexpr double kMaxGain = 15.85;       // +24 dB max lift for far-field / child voice
  constexpr double kPeakCeiling = 26000.0; // ~-2 dBFS instantaneous non-clipping cap
  constexpr double kKnee = 26000.0;
  constexpr double kHeadroom = 32767.0 - kKnee;
  for (size_t offset = 0; offset < count; offset += 160) {
    const size_t n = std::min<size_t>(160, count - offset);
    double energy = 0.0;
    double frame_peak = 0.0;
    for (size_t j = 0; j < n; ++j) {
      const double raw = static_cast<double>(samples[offset + j]);
      const double abs_raw = std::fabs(raw);
      if (abs_raw > frame_peak) frame_peak = abs_raw;
      const double x = raw / 32768.0;
      energy += x * x;
    }
    const double frame_energy = energy / static_cast<double>(n);
    // Asymmetric attack/release: fast attack (20ms) prevents first-syllable clipping when
    // speech starts after silence; slow release (250ms) prevents noise pumping between syllables.
    const double env_alpha = (frame_energy > envelope_) ? 0.50 : 0.04;
    envelope_ = (1.0 - env_alpha) * envelope_ + env_alpha * frame_energy;
    const double desired = std::clamp(kTargetRms / std::max(std::sqrt(envelope_), 1e-6),
                                      1.0, kMaxGain);
    const double gain_alpha = (desired < gain_) ? 0.50 : 0.04;
    gain_ = (1.0 - gain_alpha) * gain_ + gain_alpha * desired;
    // Instantaneous per-frame peak ceiling: never hard-clip the onset of the first syllable!
    const double peak_cap = (frame_peak > 1.0) ? (kPeakCeiling / frame_peak) : kMaxGain;
    const double applied_gain = std::min(gain_, peak_cap);
    if (applied_gain < gain_) gain_ = applied_gain;
    for (size_t j = 0; j < n; ++j) {
      double y = static_cast<double>(samples[offset + j]) * applied_gain;
      const double ay = std::fabs(y);
      if (ay > kKnee) {
        const double limited = kKnee + kHeadroom * std::tanh((ay - kKnee) / kHeadroom);
        y = (y < 0.0) ? -limited : limited;
      }
      samples[offset + j] = static_cast<int16_t>(
          std::clamp(std::nearbyint(y), -32768.0, 32767.0));
    }
  }
}

}  // namespace xiaoai_plus::audio
