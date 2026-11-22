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

Lx06SpatialBeamformer::Lx06SpatialBeamformer() {
  // Exact LX06 6-mic coordinates (meters) from Xiaomi libxaudio_engine.so
  // ParametersLoadDefault: radius 3.50 cm, clockwise 60 deg spacing starting at +150 deg.
  static constexpr std::array<std::array<double, 2>, kMics> kMicXy = {{
      {-0.03030, +0.01750},  // ch0 (+150 deg)
      { 0.00000, +0.03500},  // ch1 ( +90 deg)
      {+0.03030, +0.01750},  // ch2 ( +30 deg)
      {+0.03030, -0.01750},  // ch3 ( -30 deg)
      { 0.00000, -0.03500},  // ch4 ( -90 deg)
      {-0.03030, -0.01750},  // ch5 (-150 deg)
  }};
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kFsOverC = 16000.0 / 343.0;

  for (size_t k = 0; k < kDirs; ++k) {
    const double th = static_cast<double>(k) * (30.0 * kPi / 180.0);
    const double ux = std::cos(th);
    const double uy = std::sin(th);

    std::array<double, kMics> proj{};
    double sum_f = 0.0;
    double sum_r = 0.0;
    for (size_t m = 0; m < kMics; ++m) {
      proj[m] = kMicXy[m][0] * ux + kMicXy[m][1] * uy;
      const double d = 2.0 + proj[m] * kFsOverC;  // [0.367, 3.633] samples
      const int di = static_cast<int>(std::floor(d));
      dsb_d_int_[k][m] = di;
      dsb_frac_[k][m] = static_cast<float>(d - static_cast<double>(di));
      if (proj[m] > 0.0) sum_f += proj[m];
      if (proj[m] < 0.0) sum_r += -proj[m];
    }

    double proj_f = 0.0;
    double proj_r = 0.0;
    for (size_t m = 0; m < kMics; ++m) {
      const double wf = (proj[m] > 0.0 && sum_f > 0.0) ? (proj[m] / sum_f) : 0.0;
      const double wr = (proj[m] < 0.0 && sum_r > 0.0) ? (-proj[m] / sum_r) : 0.0;
      w_front_[k][m] = static_cast<float>(wf);
      w_rear_[k][m] = static_cast<float>(wr);
      proj_f += wf * proj[m];
      proj_r += wr * proj[m];
    }
    const double d_rear = 2.0 + (proj_f - proj_r) * kFsOverC;  // ~4.72..4.83 samples
    const int di_r = static_cast<int>(std::floor(d_rear));
    card_d_int_[k] = di_r;
    card_frac_[k] = static_cast<float>(d_rear - static_cast<double>(di_r));
  }
  Reset();
}

void Lx06SpatialBeamformer::Reset() {
  for (auto& h : hist_) h.fill(0.0f);
  short_e_.fill(1e-6);
  card_floor_.fill(1e-6);
  tv_mode_ = false;
  tv_bucket_ = 0;
  tv_leave_frames_ = 0;
  active_dir_ = 0;
  cand_dir_ = 0;
  cand_frames_ = 0;
  eq_y_ = 0.0f;
  prev_mode_card_ = false;
  prev_dir_ = 0;
  last_floor_ratio_ = 1.0;
}

void Lx06SpatialBeamformer::RenderBeam(bool use_card, int dir, size_t off, size_t n,
                                       float eq_in, float* dst, float* eq_out) const {
  const size_t k = static_cast<size_t>(dir);
  if (use_card) {
    const int di = card_d_int_[k];
    const float fr = card_frac_[k];
    const float omf = 1.0f - fr;
    const auto& wf = w_front_[k];
    const auto& wr = w_rear_[k];
    float st = eq_in;
    for (size_t i = 0; i < n; ++i) {
      const size_t t = kHist + off + i;
      const auto& xf = work_[t - 2];
      const auto& xr0 = work_[t - static_cast<size_t>(di)];
      const auto& xr1 = work_[t - static_cast<size_t>(di + 1)];
      float f_sig = 0.0f;
      float r_sig = 0.0f;
      for (size_t m = 0; m < kMics; ++m) {
        f_sig += wf[m] * xf[m];
        r_sig += wr[m] * (omf * xr0[m] + fr * xr1[m]);
      }
      st = 0.45f * (f_sig - r_sig) + 0.82f * st;
      dst[i] = st;
    }
    if (eq_out) *eq_out = st;
  } else {
    const auto& d_int = dsb_d_int_[k];
    const auto& frac = dsb_frac_[k];
    constexpr float kInvMics = 1.0f / static_cast<float>(kMics);
    for (size_t i = 0; i < n; ++i) {
      const size_t t = kHist + off + i;
      float acc = 0.0f;
      for (size_t m = 0; m < kMics; ++m) {
        const size_t idx = t - static_cast<size_t>(d_int[m]);
        const float fr = frac[m];
        acc += (1.0f - fr) * work_[idx][m] + fr * work_[idx - 1][m];
      }
      dst[i] = acc * kInvMics;
    }
    if (eq_out) *eq_out = 0.0f;
  }
}

void Lx06SpatialBeamformer::Process(const int16_t* interleaved, size_t frames, int channels,
                                    bool freeze_tv_tracker, std::vector<uint8_t>* output) {
  if (!output) return;
  if (!interleaved || frames == 0 || channels < static_cast<int>(kMics)) {
    output->clear();
    return;
  }

  work_.resize(kHist + frames);
  for (size_t i = 0; i < kHist; ++i) work_[i] = hist_[i];
  constexpr float kInvS16 = 1.0f / 32768.0f;
  const size_t stride = static_cast<size_t>(channels);
  for (size_t i = 0; i < frames; ++i) {
    const int16_t* row = interleaved + i * stride;
    auto& dst = work_[kHist + i];
    for (size_t m = 0; m < kMics; ++m) {
      dst[m] = static_cast<float>(row[m]) * kInvS16;
    }
  }
  for (size_t i = 0; i < kHist; ++i) {
    hist_[i] = work_[frames + i];
  }

  output->resize(frames * sizeof(int16_t));
  auto* out_s16 = reinterpret_cast<int16_t*>(output->data());
  std::array<float, 160> frame_new{};
  std::array<float, 160> frame_old{};

  constexpr double kTvEnterE = 4.0e-5;
  constexpr double kTvLeaveE = 1.5e-5;

  for (size_t off = 0; off < frames; off += 160) {
    const size_t n = std::min<size_t>(160, frames - off);

    // Raw spatial cardioid energy per direction (intrinsic +6 dB/oct speech-band contrast).
    std::array<double, kDirs> energies{};
    for (size_t k = 0; k < kDirs; ++k) {
      const int di = card_d_int_[k];
      const float fr = card_frac_[k];
      const float omf = 1.0f - fr;
      const auto& wf = w_front_[k];
      const auto& wr = w_rear_[k];
      double acc_e = 0.0;
      for (size_t i = 0; i < n; ++i) {
        const size_t t = kHist + off + i;
        const auto& xf = work_[t - 2];
        const auto& xr0 = work_[t - static_cast<size_t>(di)];
        const auto& xr1 = work_[t - static_cast<size_t>(di + 1)];
        float f_sig = 0.0f;
        float r_sig = 0.0f;
        for (size_t m = 0; m < kMics; ++m) {
          f_sig += wf[m] * xf[m];
          r_sig += wr[m] * (omf * xr0[m] + fr * xr1[m]);
        }
        const double d = static_cast<double>(f_sig - r_sig);
        acc_e += d * d;
      }
      energies[k] = acc_e / static_cast<double>(n);
    }

    if (!freeze_tv_tracker) {
      for (size_t k = 0; k < kDirs; ++k) {
        const double e = energies[k];
        short_e_[k] = 0.96 * short_e_[k] + 0.04 * e;
        const double alpha = (e > card_floor_[k]) ? 0.003 : 0.012;
        card_floor_[k] = (1.0 - alpha) * card_floor_[k] + alpha * e;
      }

      double max_s = short_e_[0], min_s = short_e_[0];
      double max_f = card_floor_[0], min_f = card_floor_[0];
      int anti_tv_k = 0;
      for (size_t k = 1; k < kDirs; ++k) {
        if (short_e_[k] > max_s) max_s = short_e_[k];
        if (short_e_[k] < min_s) min_s = short_e_[k];
        if (card_floor_[k] > max_f) max_f = card_floor_[k];
        if (card_floor_[k] < min_f) {
          min_f = card_floor_[k];
          anti_tv_k = static_cast<int>(k);
        }
      }
      const double ratio_s = max_s / std::max(min_s, 1e-6);
      const double ratio_f = max_f / std::max(min_f, 1e-6);
      last_floor_ratio_ = ratio_f;

      if (!tv_mode_) {
        if (max_s >= kTvEnterE && ratio_s >= 2.2) {
          tv_bucket_ = std::min(300, tv_bucket_ + 1);
        } else {
          tv_bucket_ = std::max(0, tv_bucket_ - 2);
        }
        if (tv_bucket_ >= 250 && max_f >= kTvLeaveE && ratio_f >= 1.8) {
          tv_mode_ = true;
          tv_leave_frames_ = 0;
          active_dir_ = anti_tv_k;
          cand_dir_ = anti_tv_k;
          cand_frames_ = 0;
        }
      } else {
        if (max_f < kTvLeaveE || ratio_f < 1.5) {
          ++tv_leave_frames_;
          if (tv_leave_frames_ >= 150) {
            tv_mode_ = false;
            tv_bucket_ = 0;
          }
        } else {
          tv_leave_frames_ = 0;
          if (anti_tv_k != active_dir_ &&
              card_floor_[static_cast<size_t>(anti_tv_k)] <
                  card_floor_[static_cast<size_t>(active_dir_)] * 0.85) {
            if (anti_tv_k == cand_dir_) {
              if (++cand_frames_ >= 50) {
                active_dir_ = anti_tv_k;
                cand_frames_ = 0;
              }
            } else {
              cand_dir_ = anti_tv_k;
              cand_frames_ = 1;
            }
          } else {
            cand_frames_ = 0;
          }
        }
      }
    }

    bool use_card = tv_mode_;
    if (!tv_mode_) {
      int best_k = 0;
      double best_e = energies[0];
      double min_e = energies[0];
      for (size_t k = 1; k < kDirs; ++k) {
        if (energies[k] > best_e) {
          best_e = energies[k];
          best_k = static_cast<int>(k);
        }
        if (energies[k] < min_e) min_e = energies[k];
      }
      if (best_e > 8.0e-6 && best_e > min_e * 1.35) {
        active_dir_ = best_k;
      }
    }
    const int target_dir = active_dir_;

    const bool switched = (use_card != prev_mode_card_) || (target_dir != prev_dir_);
    if (switched) {
      RenderBeam(prev_mode_card_, prev_dir_, off, n, eq_y_, frame_old.data(), nullptr);
      const float new_eq_init = (use_card && prev_mode_card_) ? eq_y_ : 0.0f;
      RenderBeam(use_card, target_dir, off, n, new_eq_init, frame_new.data(), &eq_y_);
      const float inv_n = (n > 1) ? (1.0f / static_cast<float>(n - 1)) : 1.0f;
      for (size_t i = 0; i < n; ++i) {
        const float w = static_cast<float>(i) * inv_n;
        const float blended = (1.0f - w) * frame_old[i] + w * frame_new[i];
        out_s16[off + i] = FloatToS16(blended * 32768.0f);
      }
      prev_mode_card_ = use_card;
      prev_dir_ = target_dir;
    } else {
      RenderBeam(use_card, target_dir, off, n, eq_y_, frame_new.data(), &eq_y_);
      for (size_t i = 0; i < n; ++i) {
        out_s16[off + i] = FloatToS16(frame_new[i] * 32768.0f);
      }
    }
  }
}

void KwsAgc::Process(int16_t* samples, size_t count) {
  if (!samples) return;
  constexpr double kTargetRms = 0.075;     // -22.5 dBFS target for Zipformer fbank
  constexpr double kMaxGain = 12.0;        // +21.5 dB max lift for far-field / child voice
  constexpr double kQuietMaxGain = 8.0;    // gentle cap on dead-quiet room hiss
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
    const double max_g = (envelope_ < 2.0e-6) ? kQuietMaxGain : kMaxGain;
    const double desired = std::clamp(kTargetRms / std::max(std::sqrt(envelope_), 1e-6),
                                      1.0, max_g);
    const double gain_alpha = (desired < gain_) ? 0.50 : 0.04;
    gain_ = (1.0 - gain_alpha) * gain_ + gain_alpha * desired;
    // Instantaneous per-frame peak ceiling: never hard-clip the onset of the first syllable!
    const double peak_cap = (frame_peak > 1.0) ? (kPeakCeiling / frame_peak) : max_g;
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
