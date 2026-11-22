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

// Physics-based 12-direction fractional spatial beamformer for LX06's 6-mic
// circular array (R = 3.50 cm, ch0..ch5 at +150, +90, +30, -30, -90, -150 deg,
// matching Xiaomi libxaudio_engine.so ParametersLoadDefault).
//
// - Quiet room: cardioid-energy DOA steers a 6-mic fractional Delay-and-Sum (DSB)
//   beam directly at the speaker (+7.8 dB isotropic SNR gain, flat 0..8 kHz with
//   zero Bessel J0 3.5 kHz comb null).
// - Persistent directional interferer (e.g., TV / music >= 2.5 s): automatically
//   engages an equalized front-minus-delayed-rear cardioid beam whose rear null
//   locks onto the TV direction (-12..-20 dB direct TV attenuation) while covering
//   the opposite 180..240 deg room hemisphere cleanly for both KWS and cloud uplink.
// - Smooth 10 ms linear crossfade on any direction/mode change (zero clicks).
class Lx06SpatialBeamformer {
 public:
  static constexpr size_t kMics = 6;
  static constexpr size_t kDirs = 12;   // 0, 30, 60, ..., 330 deg
  static constexpr size_t kHist = 8;    // causal history for fractional delays

  Lx06SpatialBeamformer();
  void Reset();

  // Process interleaved 16 kHz 8-ch S16_LE (ch0..ch5 = mics) into mono 16 kHz S16_LE.
  // When freeze_tv_tracker is true (speaker self-playback), directional TV floor
  // updates are paused so self-playback cannot skew the room TV direction estimate.
  void Process(const int16_t* interleaved, size_t frames, int channels,
               bool freeze_tv_tracker, std::vector<uint8_t>* output);

  bool tv_mode() const { return tv_mode_; }
  int active_dir_deg() const { return active_dir_ * 30; }
  double floor_ratio() const { return last_floor_ratio_; }

 private:
  void RenderBeam(bool use_card, int dir, size_t off, size_t n, float eq_in,
                  float* dst, float* eq_out) const;

  std::array<std::array<int, kMics>, kDirs> dsb_d_int_{};
  std::array<std::array<float, kMics>, kDirs> dsb_frac_{};
  std::array<std::array<float, kMics>, kDirs> w_front_{};
  std::array<std::array<float, kMics>, kDirs> w_rear_{};
  std::array<int, kDirs> card_d_int_{};
  std::array<float, kDirs> card_frac_{};

  std::array<std::array<float, kMics>, kHist> hist_{};
  std::vector<std::array<float, kMics>> work_;
  std::array<double, kDirs> short_e_{};
  std::array<double, kDirs> card_floor_{};
  bool tv_mode_{false};
  int tv_bucket_{0};
  int tv_leave_frames_{0};
  int active_dir_{0};
  int cand_dir_{0};
  int cand_frames_{0};
  float eq_y_{0.0f};
  bool prev_mode_card_{false};
  int prev_dir_{0};
  double last_floor_ratio_{1.0};
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
