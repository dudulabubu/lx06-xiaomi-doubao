#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xiaoai_plus::audio {

// Convert S32 capture to S16 without retaining the input. Shift 8 tests a
// right-justified 24-bit hypothesis; shift 16 maps the full S32 range.
void ConvertKwsS32ToS16(const int32_t* input, size_t samples, int shift,
                        int16_t* output);

// LX06 hardware capture decimator:
// Converts native 48 kHz 8-ch S32_LE (ch0..ch5 = 6 physical mics, ch6 = unused,
// ch7 = speaker playback loopback reference) to 16 kHz 8-ch S16_LE via a stateful
// 31-tap symmetric Kaiser low-pass FIR (fc = 7 kHz, >80 dB stopband >= 12 kHz)
// followed by a 50 Hz DC-blocking high-pass filter on ch0..ch5.
// Bypasses ALSA speexrate_medium's destructive 16-bit pre-truncation while preserving
// full 24-bit mic precision (matching Xiaomi AudioRecord_Read's >>14 mic / >>16 ref
// scaling with identical 0-sample group delay between mics and loopback reference).
class Lx06Decimator48To16 {
 public:
  static constexpr size_t kChannels = 8;
  static constexpr size_t kDecimFactor = 3;
  static constexpr size_t kTaps = 31;
  static constexpr size_t kHist = kTaps - 1;  // 30 samples continuous history

  Lx06Decimator48To16();
  void Reset();

  // Decimate interleaved 48 kHz 8-ch S32_LE to interleaved 16 kHz 8-ch S16_LE.
  // Input frame count must be a multiple of 3. Output has (in_frames / 3) * 8 int16_t.
  void Process(const int32_t* input, size_t in_frames, int mic_shift,
               std::vector<uint8_t>* output);

 private:
  // Active channels: 0..5 = mics, 6 = loopback ref (sourced from input ch7).
  std::array<std::array<float, kHist>, 7> hist_{};
  std::array<float, 6> hp_x_prev_{};
  std::array<float, 6> hp_y_prev_{};
  std::vector<float> work_;
};

// Frame-causal RMS AGC for the KWS copy only. Input must contain complete
// 10 ms frames at 16 kHz (capture uses 1440 samples = nine frames). A final
// short frame is allowed for offline EOF replay; state persists across calls.
class KwsAgc {
 public:
  void Process(int16_t* samples, size_t count);
  double gain() const { return gain_; }
 private:
  double envelope_{0.0};
  double gain_{1.0};
};

}  // namespace xiaoai_plus::audio
