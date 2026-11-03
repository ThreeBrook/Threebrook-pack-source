#!/bin/sh
# Linux x64 构建 + 单元测试（本地 CI）。
#
# 在宿主机（Windows Git Bash / Linux）上运行：用本机已有的 debian:bookworm-slim 起一个用完即删的容器，
# 项目目录挂载为 /src；apt 下载的软件包缓存在项目内 .cache/apt，第二次起可离线构建。
# 在容器内（IIS_IN_CONTAINER=1）运行：安装编译工具并构建到 build/linux-x64。
set -eu

IMAGE="${IIS_LINUX_IMAGE:-debian:bookworm-slim}"

if [ "${IIS_IN_CONTAINER:-0}" != "1" ]; then
    cd "$(dirname "$0")/.."
    root="$(pwd -W 2>/dev/null || pwd)"   # Git Bash 下取 Windows 路径给 docker
    mkdir -p .cache/apt
    # /small：1 MB tmpfs，供“真实磁盘写满”测试
    MSYS_NO_PATHCONV=1 exec docker run --rm -e IIS_IN_CONTAINER=1 -e IIS_TEST_SMALL_DIR=/small --tmpfs /small:size=1m \
        -v "$root:/src" -w /src "$IMAGE" sh scripts/ci-linux.sh
fi

# ---- 以下在容器内 ----
mkdir -p /src/.cache/apt/partial
cat > /etc/apt/apt.conf.d/90iis <<'EOF'
Dir::Cache::archives "/src/.cache/apt";
APT::Keep-Downloaded-Packages "true";
APT::Install-Recommends "false";
EOF
rm -f /etc/apt/apt.conf.d/docker-clean
apt-get update -qq
apt-get install -y -qq g++ cmake make python3 >/dev/null

cmake -S /src -B /src/build/linux-x64 -DCMAKE_BUILD_TYPE=Release
cmake --build /src/build/linux-x64 -j"$(nproc)"
ctest --test-dir /src/build/linux-x64 --output-on-failure

# 动态库只导出 iis_* 符号（CharLS / zstd 不外泄）
lib=/src/build/linux-x64/src/core/libiis.so
leaked=$(nm -D --defined-only "$lib" | awk '{print $3}' | grep -v '^iis_' || true)
if [ -n "$leaked" ]; then
    echo "libiis.so 导出了非 iis_ 符号：" >&2
    echo "$leaked" >&2
    exit 1
fi
echo "导出符号检查通过：$(nm -D --defined-only "$lib" | grep -c ' iis_') 个 iis_* 符号"
g++ --version | head -1
