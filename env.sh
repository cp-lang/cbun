# === 1. 多核并发与构建引擎 ===
export MAKEFLAGS="-j95"
export NINJA_FLAGS="-j95"
export CMAKE_BUILD_PARALLEL_LEVEL=95
export MAX_JOBS=95
export NINJA_STATUS="[%f/%t %s] "

# === 2. 编译器与链接器优化 ===
export CMAKE_GENERATOR=Ninja
export CC="clang"
export CXX="clang++"
export CFLAGS="-pipe"
export CXXFLAGS="-pipe"
export LDFLAGS="-fuse-ld=lld"

# === 3. 缓存目录控制（tmpfs 内存盘） ===
export ZIG_GLOBAL_CACHE_DIR="$(pwd)/.zig-cache/global"
export ZIG_LOCAL_CACHE_DIR="$(pwd)/.zig-cache/local"
export CCACHE_DIR="$(pwd)/.zig-cache/ccache"
export CCACHE_MAXSIZE=8G

# === 4. 工具链路径劫持与版本锁定 ===
# 注意顺序：先劫持本地 clang，再加载 zig 0.15.2，确保优先级
export PATH="$(pwd)/.local-bin:$PATH"
export PATH="/root/.local/share/zvm/0.15.2:$PATH"

# === 5. 安全自检拦截 ===
if ! mountpoint -q "$(pwd)/.zig-cache"; then
  echo "⚠️  警告：.zig-cache 未挂载 tmpfs！这会严重拖慢增量编译速度。"
  echo "请执行: sudo mount -t tmpfs -o size=20G tmpfs $(pwd)/.zig-cache"
else
  echo "✅ 内存盘检测通过，环境加载完毕。"
fi



