#!/usr/bin/env bash
# 一键在 Docker 中交叉编译 LX06 (ARMv7 arm-linux-gnueabihf) 静态二进制 xiaoai_plus_speaker
# 首次运行前请先执行一次：xiaoai-plus/toolchain/build-sherpa-static.sh 构建 Docker 镜像与 sherpa-onnx 静态库。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XIAOAI_DIR="${REPO_ROOT}/xiaoai-plus"
IMAGE_NAME="${IMAGE_NAME:-xiaoai-plus-toolchain:dev}"
BUILD_DIR="${XIAOAI_DIR}/build-armv7"
JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"

if [ ! -d "${XIAOAI_DIR}/assets/sherpa_onnx_static/lib" ]; then
  echo "未检测到 sherpa-onnx 静态库，请先运行：" >&2
  echo "  bash ${XIAOAI_DIR}/toolchain/build-sherpa-static.sh" >&2
  exit 1
fi

echo "==> 使用 Docker 镜像 ${IMAGE_NAME} 交叉编译 xiaoai_plus_speaker (jobs=${JOBS}) ..."
docker run --rm --platform linux/amd64 \
  -v "${XIAOAI_DIR}":/workspace \
  -w /workspace \
  "${IMAGE_NAME}" \
  bash -lc "
    set -e
    if [ ! -f /workspace/build-armv7/CMakeCache.txt ]; then
      SYSROOT=/opt/sysroot cmake -S /workspace -B /workspace/build-armv7 \
        -DCMAKE_TOOLCHAIN_FILE=/workspace/armv7-linux-gnueabihf.cmake \
        -DCMAKE_BUILD_TYPE=Release
    fi
    cmake --build /workspace/build-armv7 --target xiaoai_plus_speaker -j${JOBS}
    arm-linux-gnueabihf-strip --strip-unneeded /workspace/build-armv7/xiaoai_plus_speaker
    ls -lh /workspace/build-armv7/xiaoai_plus_speaker
  "

echo "==> 编译完成：${BUILD_DIR}/xiaoai_plus_speaker"
