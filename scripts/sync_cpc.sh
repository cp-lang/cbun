#!/usr/bin/env bash
set -e
# 一键编译 Rust Debug 版 cpc 并部署到 cap bin 路径
echo "[1/3] 检查 Rust 源码语法 (cargo check)..."
cargo check --manifest-path /data/dev/cp/cpc/Cargo.toml --quiet
echo "[2/3] 构建 cpc Debug 二进制 (cargo build)..."
cargo build --manifest-path /data/dev/cp/cpc/Cargo.toml --quiet
echo "[3/3] 部署 cpc 到 /data/dev/cp/cap/bin/cpc..."
cp /data/dev/cp/cpc/target/debug/cpc /data/dev/cp/cap/bin/cpc
echo "[CPC_SYNC_OK] cpc 编译器已更新并成功部署生效！"
