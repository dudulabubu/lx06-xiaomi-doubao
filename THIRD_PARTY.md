# 第三方来源与许可说明

根目录的 MIT 许可适用于本仓库中的自研与改编代码；所依赖的上游及第三方组件各自原有许可继续有效。分发编译产物时请按各自条款保留相应许可证。

## 直接上游与参考项目

| 项目 | 关系 | 许可与来源 |
| --- | --- | --- |
| [rolazd/xiaoai-doubao](https://github.com/rolazd/xiaoai-doubao) | **直接改自该项目**（音箱端 C++ 工程、O2 协议、Docker 交叉编译链、唤醒/会话状态机等） | [MIT](https://github.com/rolazd/xiaoai-doubao/blob/main/LICENSE) |
| [kslr/xiaoai-plus](https://github.com/kslr/xiaoai-plus) | 上游基座（接入豆包实时语音与本地唤醒核心能力），`xiaoai-plus/LICENSE` 原样保留 | [MIT](https://github.com/kslr/xiaoai-plus/blob/main/LICENSE) |
| [idootop/open-xiaoai](https://github.com/idootop/open-xiaoai) | 研究与刷机参考（小米音箱开放接入与固件解锁思路） | [MIT](https://github.com/idootop/open-xiaoai/blob/main/LICENSE) |
| [OwnDing/open-xiaoai](https://github.com/OwnDing/open-xiaoai) | 参考与代码来源（`xiaoai-plus/src/audio/subband_aec.*` 移植自其 `examples/xiaozhi/xiaozhi/services/audio/aec.py`） | [MIT](https://github.com/OwnDing/open-xiaoai/blob/main/LICENSE) |
| [stevenjoezhang/xiaoai-agent](https://github.com/stevenjoezhang/xiaoai-agent) | 参考项目（小米 OTA 固件解析工具思路） | [MIT](https://github.com/stevenjoezhang/xiaoai-agent) |

## 核心编译与运行依赖

| 组件 | 许可与来源 |
| --- | --- |
| sherpa-onnx | [Apache-2.0](https://github.com/k2-fsa/sherpa-onnx/blob/master/LICENSE)，v1.12.25，构建时自动拉取并静态编译 |
| KWS 模型权重与词表（`encoder.onnx` / `decoder.onnx` / `joiner.onnx` / `tokens.txt`） | [k2-fsa 官方预训练 KWS 模型](https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20.tar.bz2)；不随源码仓库分发，请自行下载并核实其模型许可 |
| nlohmann/json | [MIT](https://github.com/nlohmann/json/blob/develop/LICENSE.MIT)，v3.11.3 |
| IXWebSocket | [BSD-3-Clause](https://github.com/machinezone/IXWebSocket/blob/master/LICENSE.txt)，v11.4.5 |
| spdlog / fmt | [MIT](https://github.com/gabime/spdlog/blob/v1.x/LICENSE)，v1.15.1 |
| simple-sentencepiece / OpenFst / kaldi-native-fbank | Apache-2.0 |
| kissfft | BSD-3-Clause |
| ONNX Runtime | [MIT](https://github.com/microsoft/onnxruntime/blob/main/LICENSE) |
| WebRTC Audio Processing | [BSD-3-Clause](https://freedesktop.org/software/pulseaudio/webrtc-audio-processing/) |
| OpenSSL / zlib | Apache-2.0 / zlib |
| Mozilla CA 证书包（`ca-certificates.crt`） | [curl CA Extract](https://curl.se/docs/caextract.html)（MPL-2.0），不随源码仓库分发，请打包时自行下载 |
