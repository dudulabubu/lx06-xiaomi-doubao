#!/bin/sh
# LX06 XiaoAI + Doubao auto-boot & guardian script (/data/init.sh)
#
# 由于老款小爱音箱 Pro (LX06) 的 /data 闪存可用空间仅约 9 MB，放不下完整程序与 ONNX 模型，
# 本脚本在开机时从你的 HTTP/内网地址下载运行包到 /tmp（RAM 空间 ~120 MB）中解压并守护运行。
# /data 中仅需存放本脚本（~4 KB）与 config.ini（~1.5 KB）。
#
# 使用方法：
#   1. 将下方 PKG_URL 替换为你自己的运行包下载地址（内网 HTTP 或公网 HTTPS 均可）；
#   2. 上传到音箱 /data/init.sh 并 chmod +x /data/init.sh；
#   3. 准备好 /data/xiaoai-plus/config.ini（chmod 600）；
#   4. 在线热更新二进制后可执行：/data/init.sh --no-download（跳过下载，直接用 /tmp/xiaoai-plus 重启）。

PKG_URL="https://your-server.example.com/xiaoai-lx06.tar.gz"
RUNTIME_DIR="/tmp/xiaoai-plus"
CONFIG_DIR="/data/xiaoai-plus"
CONFIG_FILE="${CONFIG_DIR}/config.ini"
LOG_FILE="/tmp/xiaoai-plus.log"
LOCK_FILE="/tmp/xiaoai-init.pid"
NO_DOWNLOAD=0
[ "$1" = "--no-download" ] && NO_DOWNLOAD=1

# Prevent duplicate guardian instances
if [ -f "$LOCK_FILE" ]; then
    OLD_PID="$(cat "$LOCK_FILE" 2>/dev/null)"
    if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
        exit 0
    fi
fi
echo $$ > "$LOCK_FILE"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [init.sh] $1" >> "$LOG_FILE"
}

trim_log() {
    if [ -f "$LOG_FILE" ]; then
        SIZE="$(wc -c < "$LOG_FILE" 2>/dev/null || echo 0)"
        if [ "$SIZE" -gt 524288 ]; then
            tail -c 131072 "$LOG_FILE" > "${LOG_FILE}.tmp" 2>/dev/null && mv "${LOG_FILE}.tmp" "$LOG_FILE"
        fi
    fi
}

if [ ! -f "$CONFIG_FILE" ]; then
    log "missing $CONFIG_FILE, exiting"
    exit 1
fi

chmod 700 "$CONFIG_DIR" 2>/dev/null || true
chmod 600 "$CONFIG_FILE" 2>/dev/null || true
mkdir -p "${CONFIG_DIR}/usage" 2>/dev/null || true

log "boot script started, waiting for mipns-xiaomi and network..."

# Wait for native mipns-xiaomi & audio subsystem to settle
i=0
while [ $i -lt 30 ]; do
    if pidof mipns-xiaomi >/dev/null 2>&1; then
        break
    fi
    sleep 2
    i=$((i + 1))
done
[ "$NO_DOWNLOAD" -eq 0 ] && sleep 5

# Download runtime package into /tmp with retries (skipped on live hot-restart)
DOWNLOADED=0
if [ "$NO_DOWNLOAD" -eq 1 ] && [ -x "${RUNTIME_DIR}/xiaoai_plus_speaker" ]; then
    DOWNLOADED=1
    log "using existing runtime package in $RUNTIME_DIR (--no-download)"
else
    TMP_TGZ="/tmp/xiaoai-lx06.tar.gz.part"
    ATTEMPT=0
    while [ $ATTEMPT -lt 60 ]; do
        ATTEMPT=$((ATTEMPT + 1))
        rm -f "$TMP_TGZ"
        if curl -fsSL --connect-timeout 10 --max-time 60 "$PKG_URL" -o "$TMP_TGZ" 2>>"$LOG_FILE"; then
            rm -rf "${RUNTIME_DIR}.new"
            mkdir -p "${RUNTIME_DIR}.new"
            if tar -xzf "$TMP_TGZ" -C "${RUNTIME_DIR}.new" 2>>"$LOG_FILE" && [ -x "${RUNTIME_DIR}.new/xiaoai_plus_speaker" ]; then
                rm -f "$TMP_TGZ"
                rm -rf "$RUNTIME_DIR"
                mv "${RUNTIME_DIR}.new" "$RUNTIME_DIR"
                DOWNLOADED=1
                log "downloaded and extracted runtime package on attempt $ATTEMPT"
                break
            else
                log "tar extract failed on attempt $ATTEMPT, retrying..."
                rm -rf "${RUNTIME_DIR}.new" "$TMP_TGZ"
            fi
        else
            log "download failed on attempt $ATTEMPT, retrying in 5s..."
        fi
        sleep 5
    done
fi

if [ "$DOWNLOADED" -ne 1 ]; then
    log "failed to download runtime package after $ATTEMPT attempts, giving up"
    exit 1
fi

# Guardian loop: restart on unexpected exit, stop if crashing >= 5 times in 10 minutes
CRASH_COUNT=0
WINDOW_START="$(date +%s)"

while true; do
    trim_log
    log "starting xiaoai_plus_speaker (nice -10)"
    cd "$RUNTIME_DIR" || exit 1
    # 与原厂 mipns-xiaomi 使用相同的 nice -10 调度优先级，防止 48kHz 8声道录音 overrun
    nice -n -10 ./xiaoai_plus_speaker -c "$CONFIG_FILE" >> "$LOG_FILE" 2>&1
    RC=$?
    NOW="$(date +%s)"
    log "xiaoai_plus_speaker exited with rc=$RC"

    if [ $((NOW - WINDOW_START)) -gt 600 ]; then
        WINDOW_START="$NOW"
        CRASH_COUNT=1
    else
        CRASH_COUNT=$((CRASH_COUNT + 1))
    fi

    if [ "$CRASH_COUNT" -ge 5 ]; then
        log "crashed $CRASH_COUNT times within 10 minutes; stopping guardian"
        exit 1
    fi

    sleep 5
done
