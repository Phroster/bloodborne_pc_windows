#!/usr/bin/env bash
# bbport-windows: build in an MSYS2 CLANG64 shell (see README.md).
# Builds out/bb-probe.exe: the loader and runtime with the GPU library.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
if [[ ${MSYSTEM:-} != CLANG64 ]]; then
    echo 'Run this from an MSYS2 CLANG64 shell (C:\msys64\clang64.exe).' >&2; exit 1
fi
mkdir -p out
# Submodules and this port's changes to FSR-Vulkan, as build.sh does.
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_LTO="${BB_LTO:-OFF}" \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -Wno-author >/dev/null
echo "GPU library: LTO ${BB_LTO:-OFF}"
if ! ninja -C out/gpu bb-probe > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo "Build failed (full log: out/gpu-build.log)" >&2; exit 1
fi
echo "Built $PWD/out/bb-probe.exe"
