#!/bin/bash
#
# iFlytek iclass-c8pro kernel build script
# Kernel: Linux 4.14, Arch: arm64
#

set -e

KERNEL_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN_DIR="${KERNEL_DIR}/../toolchain"
CROSS_COMPILE="${TOOLCHAIN_DIR}/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-"
DEFCONFIG="iflytek_iclass-c8pro_defconfig"

export ARCH="arm64"
export CROSS_COMPILE

JOBS="${JOBS:-$(nproc)}"

usage() {
    cat <<EOF
Usage: $0 [defconfig|build|clean|distclean|rebuild|Image|dtbs|modules]

Commands:
  defconfig    Generate/update .config from ${DEFCONFIG}
  build        编译内核 (默认)
  rebuild      clean + 编译
  Image        只编译 Image
  dtbs         只编译设备树
  modules      编译内核模块
  clean        清理中间文件
  distclean    深度清理
EOF
    exit 0
}

do_defconfig() {
    echo "=== 生成 .config: ${DEFCONFIG} ==="
    make O="${KERNEL_DIR}" -C "${KERNEL_DIR}" "${DEFCONFIG}"
    make O="${KERNEL_DIR}" -C "${KERNEL_DIR}" olddefconfig
}

do_build() {
    echo "=== 开始编译内核 ==="
    echo "  源码: ${KERNEL_DIR}"
    echo "  工具链: ${CROSS_COMPILE}"
    echo "  并行任务: ${JOBS}"
    echo "---"
    make O="${KERNEL_DIR}" -C "${KERNEL_DIR}" -j"${JOBS}"
    echo "=== 编译完成 ==="
    echo "  Image:   ${KERNEL_DIR}/arch/arm64/boot/Image"
    echo "  DTB:     ${KERNEL_DIR}/arch/arm64/boot/dts/sprd/"
}

do_clean() {
    echo "=== clean ==="
    make O="${KERNEL_DIR}" -C "${KERNEL_DIR}" clean
}

do_distclean() {
    echo "=== distclean ==="
    make O="${KERNEL_DIR}" -C "${KERNEL_DIR}" distclean
}

do_Image() {
    echo "=== 只编译 Image ==="
    make O="${KERNEL_DIR}" -C "${KERNEL_DIR}" -j"${JOBS}" Image
}

do_dtbs() {
    echo "=== 只编译设备树 ==="
    make O="${KERNEL_DIR}" -C "${KERNEL_DIR}" -j"${JOBS}" dtbs
}

do_modules() {
    echo "=== 编译内核模块 ==="
    make O="${KERNEL_DIR}" -C "${KERNEL_DIR}" -j"${JOBS}" modules
}

# --------------------------------------------------------------------
MAKE_CMD="build"
if [[ $# -gt 0 ]]; then
    case "$1" in
        -h|--help|help) usage ;;
        defconfig) MAKE_CMD="defconfig" ;;
        build)     MAKE_CMD="build" ;;
        rebuild)   MAKE_CMD="rebuild" ;;
        Image)     MAKE_CMD="Image" ;;
        dtbs)      MAKE_CMD="dtbs" ;;
        modules)   MAKE_CMD="modules" ;;
        clean)     MAKE_CMD="clean" ;;
        distclean) MAKE_CMD="distclean" ;;
        *) echo "未知命令: $1"; usage ;;
    esac
fi

case "$MAKE_CMD" in
    rebuild)
        do_clean
        do_defconfig
        do_build
        ;;
    build)
        do_defconfig
        do_build
        ;;
    defconfig) do_defconfig ;;
    clean)     do_clean ;;
    distclean) do_distclean ;;
    Image)     do_Image ;;
    dtbs)      do_dtbs ;;
    modules)   do_modules ;;
esac
