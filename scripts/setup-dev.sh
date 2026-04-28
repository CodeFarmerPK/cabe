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
    gcc                      # GCC 15
    gcc-c++
    clang                    # Clang 20
    libasan                  # AddressSanitizer runtime
    libtsan                  # ThreadSanitizer runtime
    libubsan                 # UBSanitizer runtime
    cmake                    # 3.30+
    make
    ninja-build
    pkgconf-pkg-config       # CMake pkg_check_modules 入口
    git
    tar
    kernel-headers
    util-linux               # losetup
    gtest-devel
    gmock-devel
    google-benchmark-devel   # P1+: 微基准
    liburing                 # P4: io_uring runtime
    liburing-devel           # P4: io_uring headers + pkg-config metadata (>= 2.9 校验在下方)
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

if [[ "$CI_MODE" == "false" ]]; then
    echo ">>> Installing dev-only profiling tools..."
    $SUDO dnf install -y "${DEV_EXTRA_PKGS[@]}" || true
fi

# ---------- P4 io_uring runtime checks ----------
echo ""
echo ">>> Verifying io_uring runtime prerequisites (P4)..."

# 1. liburing version (CMake will require >= 2.9, see doc/p4_io_uring_design.md D12)
if ! pkg-config --atleast-version=2.9 liburing; then
    echo "Error: liburing >= 2.9 required for P4 io_uring backend (found: $(pkg-config --modversion liburing 2>/dev/null || echo missing))" >&2
    exit 1
fi
echo "    liburing: $(pkg-config --modversion liburing)"

# 2. kernel io_uring not disabled by sysctl (Linux 6.6+)
if [[ -r /proc/sys/kernel/io_uring_disabled ]]; then
    case "$(cat /proc/sys/kernel/io_uring_disabled)" in
        0) echo "    io_uring: enabled" ;;
        1) echo "    io_uring: restricted (CAP_SYS_ADMIN required); P4 tests must run as root or capability-granted user" ;;
        2) echo "Error: io_uring disabled by sysctl (kernel.io_uring_disabled=2). Run: sudo sysctl -w kernel.io_uring_disabled=0" >&2; exit 1 ;;
        *) echo "    io_uring: unknown disabled state, assuming enabled" ;;
    esac
else
    echo "    io_uring: sysctl absent (kernel < 6.6), assumed enabled"
fi

# 3. RLIMIT_MEMLOCK advisory (P4 register_buffers pins pool memory; D15)
MEMLOCK=$(ulimit -l)
if [[ "$MEMLOCK" == "unlimited" ]]; then
    echo "    RLIMIT_MEMLOCK: unlimited"
elif (( MEMLOCK < 16384 )); then
    cat >&2 <<EOF
Warning: ulimit -l = ${MEMLOCK} KB is below P4 io_uring needs.
  register_buffers pins ~16 MiB for default buffer_pool_count=16.
  Recommended:
    ulimit -l unlimited                   # current shell
    LimitMEMLOCK=infinity                 # systemd unit
EOF
else
    echo "    RLIMIT_MEMLOCK: ${MEMLOCK} KB (sufficient for default pool of 16)"
fi

echo ""
echo "=== Toolchain versions ==="
gcc --version | head -n1
command -v clang &>/dev/null && clang --version | head -n1
cmake --version | head -n1
echo "kernel:   $(uname -r)"
echo "liburing: $(pkg-config --modversion liburing 2>/dev/null || echo missing)"
echo ""
echo "Next:"
echo "  ./scripts/run-tests.sh --asan"
echo "  ./scripts/run-tests.sh --backend=io_uring   # P4 io_uring path (after M1)"