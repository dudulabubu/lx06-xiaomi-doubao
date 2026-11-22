# lx06-xiaomi-doubao

让**老款小米小爱音箱 Pro（型号 `LX06`，黑色立式 6 麦版）**原生接入**豆包端到端实时语音（火山引擎 O2 协议 + 联网搜索）**，同时完整保留原版“小爱同学”双唤醒共存、远场 6 麦全精度拾音、播报中打断继续听、喊“小爱同学”秒退豆包模式。

> ⚠️ **风险提示**：改造音箱需要开启 SSH（通常涉及刷写解锁分区），存在一定风险且会失去官方保修。本仓库**仅包含开源音箱端源码、配置模板与脚本，不分发任何小米原厂固件、模型权重、证书或 API 密钥**，请自行评估并承担操作风险。

---

## 项目来源与参考致谢

本项目并非从零编写，而是在以下优秀开源项目基础上针对 **老款小爱音箱 Pro（LX06）** 进行深度移植、音频链路重构与中文日常体验优化而来：

- **直接改自（上游基座）**：[rolazd/xiaoai-doubao](https://github.com/rolazd/xiaoai-doubao)（MIT License）
  - 继承了其 `xiaoai-plus` C++ 架构、火山引擎 O2/V3 实时语音客户端、Docker ARMv7 静态交叉编译链、本地 Zipformer KWS 观测补丁、会话门控状态机等核心框架。
- **核心底层来源**：[kslr/xiaoai-plus](https://github.com/kslr/xiaoai-plus)（MIT License）
  - 音箱端实时语音接入与本地唤醒词架构的最初源头。
- **重要参考与组件来源**：
  - [idootop/open-xiaoai](https://github.com/idootop/open-xiaoai)（MIT License）：小米音箱 SSH 解锁与开放改造思路参考。
  - [OwnDing/open-xiaoai](https://github.com/OwnDing/open-xiaoai)（MIT License）：`xiaoai-plus/src/audio/subband_aec.*` 线性子带回声消除算法移植自其 `aec.py`。
  - [k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx)（Apache-2.0）：本地离线唤醒词引擎与中英 Zipformer KWS 模型。
  - [stevenjoezhang/xiaoai-agent](https://github.com/stevenjoezhang/xiaoai-agent)（MIT License）：小米 OTA 固件解包工具参考。

详细第三方依赖与许可证见 [THIRD_PARTY.md](THIRD_PARTY.md)。

---

## 适配设备与已验证环境

- **已验证设备**：老款 **小米小爱音箱 Pro（`LX06`）**，固件版本 `1.94.13`（6 麦环形阵列，Amlogic 64-bit 内核 + ARMv7 hf 用户态，`/data` 剩余约 9 MB，`/tmp` RAM 约 120 MB）。
- **已验证编译环境**：Windows 11 + Docker Desktop / WSL2（`linux/amd64` 容器交叉编译 `arm-linux-gnueabihf` 全静态二进制，Linux / macOS 同样适用）。

---

## 相对上游做了哪些核心改动与增强？

### 1. 老款 LX06 底层音频链路彻底重构（解决远场与儿童听不清）
深入分析 LX06 `/etc/asound.conf` 与原厂 `libxaudio_engine.so` 后，修复了直接跑上游代码在 LX06 上的两个致命硬件不兼容问题（详见 [docs/LX06_AUDIO_ANALYSIS.md](docs/LX06_AUDIO_ANALYSIS.md)）：
- **修复 `ch3` 错把 4 号麦克风当回采、WebRTC 把人声自己抵消掉的致命 Bug**：LX06 为 8 声道硬件（`ch0~ch5` 6 颗实体麦，`ch7` 功放回采），已改为准确使用 **`ch7`** 作为回声参考。
- **绕开 ALSA `speexrate_medium` 16-bit 预截断**：改为硬件原生 **`48000 Hz / 8 ch / S32_LE`** 采集，在应用层通过 **31 阶对称 Kaiser 低通 FIR** 在 24-bit 浮点精度下 3:1 降采样到 16 kHz + 50 Hz 去直流滤波，远场与儿童轻声的清晰度质变。
- **6 麦环形阵列物理空间波束成形 + 自动抗电视心形零点**：直接提取小米原厂 `libxaudio_engine.so` 中 LX06 精确 6 麦坐标（`R = 3.50 cm`，60° 均匀分布），实现 12 方向（每 30°）分数延迟波束成形：
  - 安静房间：基于心形空间能量 DOA 指向说话人，输出分数延迟 `DSB` 波束（+7.8 dB 增益，彻底消除普通 6 麦均值在 3.5 kHz 的 `J₀` 梳状零点）；
  - 看电视 / 持续方向性噪声环境（≥ 2.5 秒）：自动锁定电视方向并平滑切换为**抗电视心形波束**，将空间零点精准对准电视（实测 0 dB / -3 dB 电视噪比下唤醒率由旧版 3~7% 跃升至 77~93%），同时兼顾云端对话上行清晰度。
- **重配 WebRTC 语音前端**：启用 80 Hz HPF + `kModerate` 降噪 + `kAdaptiveDigital` AGC（最高 `+18 dB` 语音自适应增益 + 限幅器），短句、远场与儿童声音更饱满。
- **彻底消除录音 `overrun`**：扩容 `arecord` pipe 至 1 MB + 异步队列线程解耦 + `nice -n -10` 启动优先级。

### 2. 唤醒灵敏度、抗电视误唤醒与播报中打断优化
- **修复 `KwsAgc` 慢攻击首字削波**：增加每 10 ms 帧前瞻峰值限幅 + 20 ms 极速压限 + `tanh` 软拐点 + 静音温和限幅，从安静中突然喊“豆包豆包”第一声“豆”零削波。
- **纯 8 音素完整唤醒词表 + `chunk-16` 模型升级**：严格避免在 `sherpa-onnx` 上下文图中混入半词前缀（否则第一遍“豆包”会中途重置编码器毁掉第二遍并造成电视高频误唤醒），改用经过多音色/远场/电视干扰实测标定的 8 组完整 8 音素词表（标准 + 轻声 + 轻度方言/儿童声调容错），配合 `chunk-16` Zipformer 模型（CPU 降低 ~35%~40%，上下文视野翻倍），实现高唤醒率与极低电视误唤醒。
- **播报中喊“豆包豆包”打断并继续听**：喇叭出声时自动启用线性多抽头 `SubbandAec`（利用真实 `ch7` 回采消除 23~30 dB 外放声且不压人声），打断后立刻停播、“叮”一声亮蓝灯继续监听下一句并保持同一段上下文（同时修复打断后 `pending_playback_chunks_` 残留与 O2 协议重入问题）。

### 3. 中文日常助手与 LX06 系统体验完善
- **通用中文日常对话 + 联网搜索**：移除上游纯英文儿童系统提示词限制，新增火山引擎 O2 协议联网搜索配置项（`websearch_api_key` / `websearch_type`）。
- **LX06 原生播放、静默启动与三态灯环联动**：输出 PCM 改为 `default`（LX06 无 `Playback`）、静默启动不打扰、接入 `ubus call led` 三态灯环（聆听 / 思考 / 播报）。
- **智能双模式唤醒**：喊完“豆包豆包”直接紧跟提问会预缓冲语音并跳过“在呢”立刻回答；只喊“豆包豆包”不说话则正常回复“在呢”等待提问。
- **原生“小爱同学”无缝共存与秒退**：适配 LX06 `/tmp/log/messages` 真实日志（LX06 无 `wakeup.sh`），豆包说话/待命期间喊“小爱同学”可在 ~80 ms 内停播、交还灯环并退出豆包模式；2 秒后恢复双唤醒待命。
- **播放响度与长故事防截断**：新增 `tanh` 软限幅支持 `playback_gain = 2.60` 放大不破音，播放队列扩容至 512 块（讲 3 分钟长故事不断尾）。
- **适配 LX06 极小 `/data` 闪存的 RAM 运行方案**：`/data` 仅存 `init.sh` + `config.ini`（约 5 KB），开机自动拉取运行包到 `/tmp`（RAM）解压守护运行。

---

## 目录结构

```text
lx06-xiaomi-doubao/
├── README.md                          # 本说明文档
├── LICENSE                            # MIT 许可证（保留上游版权）
├── THIRD_PARTY.md                     # 第三方组件来源与许可说明
├── docs/
│   └── LX06_AUDIO_ANALYSIS.md         # LX06 6 麦音频架构与优化原理深度分析
├── scripts/
│   ├── build_lx06.sh                  # Docker ARMv7 一键交叉编译脚本
│   └── init_lx06.sh                   # LX06 开机自启与 RAM 守护脚本模板（/data/init.sh）
└── xiaoai-plus/                       # 音箱端 C++ 完整源码
    ├── CMakeLists.txt
    ├── armv7-linux-gnueabihf.cmake    # ARMv7 交叉编译工具链定义
    ├── config.ini.example             # LX06 中文联网版配置模板（不含密钥）
    ├── keywords/
    │   └── keywords_doubao_repeat_end.txt  # 优化后的双层唤醒词 + 退出词表
    ├── src/                           # 音箱端 C++ 源码
    ├── tests/                         # 单元测试源码
    └── toolchain/                     # Docker 构建与 sherpa-onnx 静态编译脚本/补丁
```

---

## 快速上手（编译与部署）

> 前提：你的 LX06 已通过拆机/刷机开启 SSH（参考 `idootop/open-xiaoai` 或 `rolazd/xiaoai-doubao` 的固件解锁原理），并已在火山引擎开通豆包实时语音 API Key（可选开通联网搜索 Key）。

### 第一步：构建交叉编译镜像与 `sherpa-onnx` 静态库（仅首次需要）

需要本机已安装 Docker（Windows 推荐 Docker Desktop + WSL2）：

```bash
bash xiaoai-plus/toolchain/build-sherpa-static.sh
```

该脚本会自动构建 `xiaoai-plus-toolchain:dev` 镜像、拉取 `sherpa-onnx v1.12.25`、应用唤醒得分观测补丁并静态编译到 `xiaoai-plus/assets/sherpa_onnx_static/`。

### 第二步：编译 ARMv7 音箱端可执行文件

```bash
bash scripts/build_lx06.sh
```

编译产物位于 `xiaoai-plus/build-armv7/xiaoai_plus_speaker`（全静态链接 ARMv7 ELF，约 19 MB）。

### 第三步：准备运行时资源包（模型与根证书）

由于模型权重和 CA 证书不随本源码仓库分发，请自行下载并打包成 `xiaoai-lx06.tar.gz`：

1. 下载官方 Zipformer 中英 KWS 模型（[sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20.tar.bz2](https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20.tar.bz2)），提取 **`chunk-16`** 版本（相比 `chunk-8` 上下文视野翻倍且 CPU 占用低约 35%）：
   - `encoder-epoch-13-avg-2-chunk-16-left-64.int8.onnx` → 重命名为 `assets/encoder.onnx`
   - `decoder-epoch-13-avg-2-chunk-16-left-64.onnx` → 重命名为 `assets/decoder.onnx`
   - `joiner-epoch-13-avg-2-chunk-16-left-64.int8.onnx` → 重命名为 `assets/joiner.onnx`
   - `tokens.txt` → `assets/tokens.txt`
2. 下载 Mozilla CA 根证书包（[curl cacert.pem](https://curl.se/ca/cacert.pem)）→ 重命名为 `assets/ca-certificates.crt`。
3. 组织如下目录并打包为 `xiaoai-lx06.tar.gz`（约 14 MB）：

```text
xiaoai-lx06.tar.gz
├── xiaoai_plus_speaker
├── assets/
│   ├── encoder.onnx
│   ├── decoder.onnx
│   ├── joiner.onnx
│   ├── tokens.txt
│   └── ca-certificates.crt
└── keywords/
    └── keywords_doubao_repeat_end.txt
```

### 第四步：部署到 LX06 音箱

由于 LX06 的 `/data` 分区剩余空间仅约 9 MB，推荐采用 **“配置留 `/data`、程序进 `/tmp`（RAM）”** 的部署方式：

1. 将打包好的 `xiaoai-lx06.tar.gz` 放到你局域网的任意 HTTP 服务（如 NAS、路由器、OpenWrt、Nginx）或个人文件服务器。
2. 复制 `xiaoai-plus/config.ini.example` 为本地 `config.ini`，填入你的火山引擎 `api_key`（以及可选的 `websearch_api_key`）并按需定制 `system_role`。
3. 上传 `config.ini` 到音箱 `/data/xiaoai-plus/config.ini`，设置严格权限：
   ```sh
   mkdir -p /data/xiaoai-plus
   chmod 700 /data/xiaoai-plus
   chmod 600 /data/xiaoai-plus/config.ini
   ```
4. 修改 `scripts/init_lx06.sh` 中的 `PKG_URL` 为你的 `xiaoai-lx06.tar.gz` 下载地址，上传到音箱 `/data/init.sh` 并赋予执行权限（`chmod +x /data/init.sh`），并在音箱的开机自启钩子中后台调用 `/data/init.sh &`。
5. 手动启动测试：
   ```sh
   /data/init.sh &
   tail -f /tmp/xiaoai-plus.log
   ```

---

## 日常使用说明

- **唤醒豆包**：喊 **“豆包豆包”**
  - 喊完停住 → 回复“在呢”，蓝灯常亮等待提问；
  - 喊完直接连着问（如“豆包豆包，今天天气怎么样”）→ 跳过“在呢”直接回答。
- **播报中打断继续问**：豆包说话时再喊 **“豆包豆包”** → 立刻停播、“叮”一声继续听新问题（保留上下文）。
- **调音量**：按音箱顶部 `+` / `-` 键，或直接说“大声一点 / 小声一点”。
- **退出豆包**：说 **“再见 / 拜拜 / 退下吧 / 停止 / 关闭”**，或超过 `idle_seconds`（默认 120 秒）无对话自动退出。
- **切换回原版小爱**：任何时候（包括豆包正在说话时）直接喊 **“小爱同学”** → 豆包自动停播退出，交给原版小爱接管；2 秒后可再次喊“豆包豆包”。

---

## 常见问题

- **Q：Windows 上执行 `build-sherpa-static.sh` 提示 patch 失败？**
  - 本仓库已配置 `.gitattributes` 强制 LF 换行并在脚本中添加了 `git apply --ignore-whitespace`，请确保不要用会强制转 CRLF 的工具覆盖 `.patch` 文件。
- **Q：想把远场或儿童灵敏度再调高/调低？**
  - 无需重新编译，直接修改音箱 `/data/xiaoai-plus/config.ini` 中的 `s32_shift`：`14` 与小米原厂一致、`13` 更灵敏（默认推荐）、`12` 极高灵敏度；修改后重启进程即可。
- **Q：如何查看各通道麦克风原始精度和是否丢帧？**
  - 在音箱 `/tmp/xiaoai-plus` 目录下运行 `./xiaoai_plus_speaker --kws-only --kws-diagnostics --kws-s32-shift 13 --keywords-file keywords/keywords_doubao_repeat_end.txt`，每 5 秒会输出 8 通道原始精度、降采样电平、耗时与 `overrun_lines`。

---

## 许可与免责声明

- 本项目以 **MIT License** 开源，详见 [LICENSE](LICENSE)。
- 本项目为个人开源实验与学习成果，与小米、字节跳动、火山引擎无任何关联；“小爱同学”“豆包”均为各自所属公司的商标。
- 请勿将包含个人 `api_key` 的 `config.ini` 提交到任何公开仓库。
