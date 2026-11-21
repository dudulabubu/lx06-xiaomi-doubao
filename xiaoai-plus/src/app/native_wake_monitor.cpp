#include "app/native_wake_monitor.hpp"

#include <fstream>
#include <sys/stat.h>

namespace xiaoai_plus::app {

NativeWakeMonitor::NativeWakeMonitor(std::function<void()> on_wake, std::string path)
    : on_wake_(std::move(on_wake)), path_(std::move(path)) {}

NativeWakeMonitor::~NativeWakeMonitor() { Stop(); }

void NativeWakeMonitor::Start() {
  stop_.store(false);
  thread_ = std::thread([this] { Loop(); });
}

void NativeWakeMonitor::Stop() {
  stop_.store(true);
  if (thread_.joinable()) thread_.join();
}

bool NativeWakeMonitor::Blocked() const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto now = Clock::now();
  if (native_active_ && now < active_until_) return true;
  return now < hold_until_;
}

void NativeWakeMonitor::ObserveLine(const std::string& line) {
  // Real wake events on LX06 (libxaudio_engine / mico_aivs_lab) and OH2P (mipns-xiaomi / wakeup.sh).
  const bool from_mipns = line.find("mipns-xiaomi[") != std::string::npos;
  const bool from_aivs = line.find("mico_aivs_lab[") != std::string::npos;
  const bool pns_wake = from_mipns &&
                        (line.find("real wakeup--") != std::string::npos ||
                         line.find("send real_wakeup event") != std::string::npos);
  const bool aivs_wake = from_aivs &&
                         line.find("WAKEUP_REAL:") != std::string::npos;
  const bool native_asr_start = line.find("wakeup.sh: asrstart") != std::string::npos;

  if (pns_wake || aivs_wake || native_asr_start) {
    bool fire = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      const auto now = Clock::now();
      const bool dup = last_wake_at_.time_since_epoch().count() != 0 &&
                       (now - last_wake_at_) < std::chrono::seconds(3);
      if (!dup) {
        fire = true;
        last_wake_at_ = now;
      }
      // Immediately preempt Doubao via on_wake_(), then keep only a brief 2s
      // guard so the user can switch back to "豆包豆包" right after XiaoAI without
      // suffering a 15-second deaf window.
      native_active_ = false;
      active_until_ = {};
      hold_until_ = now + std::chrono::seconds(2);
    }
    if (fire && on_wake_) on_wake_();
    return;
  }

  const bool pns_finish = from_mipns &&
                          (line.find("dialog finish!") != std::string::npos ||
                           line.find("asr timeout!") != std::string::npos);
  const bool native_asr_end = line.find("wakeup.sh: asrend") != std::string::npos;

  if (pns_finish || native_asr_end) {
    std::lock_guard<std::mutex> lock(mu_);
    native_active_ = false;
    active_until_ = {};
  }
}

void NativeWakeMonitor::Loop() {
  ino_t inode = 0;
  std::streamoff offset = 0;
  std::string partial;
  while (!stop_.load()) {
    struct stat st {};
    if (stat(path_.c_str(), &st) == 0) {
      if (inode == 0) {
        inode = st.st_ino;
        offset = st.st_size;  // Ignore old or rotated history.
        partial.clear();
      } else if (inode != st.st_ino || st.st_size < offset) {
        inode = st.st_ino;
        offset = 0;  // Inspect new lines written during rotation.
        partial.clear();
      } else if (st.st_size > offset) {
        std::ifstream file(path_, std::ios::binary);
        if (file) {
          file.seekg(offset);
          char buf[4096];
          while (file && !stop_.load()) {
            file.read(buf, sizeof(buf));
            const auto n = file.gcount();
            if (n <= 0) break;
            offset += n;
            partial.append(buf, static_cast<size_t>(n));
            size_t end;
            while ((end = partial.find('\n')) != std::string::npos) {
              ObserveLine(partial.substr(0, end));
              partial.erase(0, end + 1);
            }
            if (partial.size() > 16384) partial.clear();
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
}

}  // namespace xiaoai_plus::app
