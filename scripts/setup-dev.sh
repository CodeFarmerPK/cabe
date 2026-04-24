#!/usr/bin/env bash
# ============================================================
# Cabe dev environment bootstrap —— Fedora 43 only.
#
# Usage:
#   ./scripts/setup-dev.sh          # full dev install (local machine)
#   ./scripts/setup-dev.sh --ci     # minimal install for CI containers
#
# Fails fast if not on Fedora 43+.
# ============================================================
set -euo pipefail

# ---------- arg parsing ----------
CI_MODE=false
for arg in "$@"; do
    case "$arg" in
        --ci)      CI_MODE=true ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *)         echo "Unknown arg: $arg" >&2; exit 1 ;;
    esac
done

# ---------- distro check ----------
if [[ ! -f /etc/os-release ]]; then
    echo "Error: /etc/os-release not found. Cabe requires Fedora 43." >&2
    exit 1
fi
# shellcheck disable=SC1091
. /etc/os-release

if [[ "${ID:-}" != "fedora" ]]; then
    echo "Error: Cabe only supports Fedora. Detected: ${ID:-unknown} ${VERSION_ID:-?}" >&2
    exit 1
fi
if (( ${VERSION_ID:-0} < 43 )); then
    echo "Error: Cabe requires Fedora 43+. Detected: Fedora $VERSION_ID" >&2
    exit 1
fi
echo ">>> Fedora $VERSION_ID detected (CI mode: $CI_MODE)"

# ---------- privilege ----------
SUDO=""
[[ $EUID -ne 0 ]] && SUDO="sudo"

# ---------- required packages (每个 build 都需要) ----------
REQUIRED_PKGS=(
    gcc                 # GCC 15
    gcc-c++
    clang               # Clang 20
    libasan          # AddressSanitizer runtime
    libtsan          # ThreadSanitizer runtime
    libubsan         # UBSanitizer runtime
    cmake               # 3.30+
    make
    ninja-build
    pkgconf-pkg-config
    git
    tar
    kernel-headers
    util-linux          # losetup
    gtest-devel
    gmock-devel
    liburing
    liburing-devel
)

# ---------- future-P 依赖（现在装好，进入 P1~P3 时不卡壳） ----------
FUTURE_PKGS=(
    google-benchmark-devel   # P1 微基准
    liburing-devel           # P3 io_uring
)

# ---------- dev-only 工具（CI 跳过以加速） ----------
DEV_EXTRA_PKGS=(
    perf
    bpftrace
    fio
    strace
    lsof
    valgrind
    htop
    hyperfine
)

echo ">>> Installing required packages..."
$SUDO dnf install -y "${REQUIRED_PKGS[@]}"

echo ">>> Installing future-P dependencies..."
$SUDO dnf install -y "${FUTURE_PKGS[@]}" || {
    echo "Warning: some future-P packages unavailable. CMake FetchContent can fall back." >&2
}

if [[ "$CI_MODE" == "false" ]]; then
    echo ">>> Installing dev-only profiling tools..."
    $SUDO dnf install -y "${DEV_EXTRA_PKGS[@]}" || true
fi

echo ""
echo "=== Toolchain versions ==="
gcc --version | head -n1
command -v clang &>/dev/null && clang --version | head -n1
cmake --version | head -n1
echo "kernel: $(uname -r)"
echo ""
echo "Next:"
echo "  ./scripts/run-tests.sh --asan"