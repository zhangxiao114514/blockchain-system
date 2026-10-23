#!/usr/bin/env bash
# build.sh — 编译 C++ 共识内核
#
# 构建策略（按优先级）：
#   1. CMake + MinGW Makefiles / Ninja / Make（推荐，自动使用 64 位工具链）
#   2. 直接调用 g++（无 cmake 或无 make 时）
#
# 注意：MinGW 的 make 无法处理含中文的绝对路径（会按 ANSI 码页解析 Makefile 而乱码），
# 因此当工程路径包含非 ASCII 字符时，脚本会把源码暂存到临时 ASCII 目录构建，
# 再把产物复制回 bin/。工程本身无需任何改动。
#
#   ./build.sh              自动选择
#   USE_CMAKE=0 ./build.sh  强制使用 g++
set -e
cd "$(dirname "$0")"
ROOT="$(pwd)"
mkdir -p bin

# ---------- 定位 cmake ----------
CMAKE_BIN=""
if [ "$USE_CMAKE" != "0" ]; then
  if command -v cmake >/dev/null 2>&1; then
    CMAKE_BIN="$(command -v cmake)"
  elif [ -x "/c/Program Files/CMake/bin/cmake.exe" ]; then
    CMAKE_BIN="/c/Program Files/CMake/bin/cmake.exe"
  elif [ -x "/c/Program Files (x86)/CMake/bin/cmake.exe" ]; then
    CMAKE_BIN="/c/Program Files (x86)/CMake/bin/cmake.exe"
  fi
fi

# ---------- 优先使用 64 位 MinGW 工具链 ----------
for d in "/c/Program Files/mingw64/bin" "/c/msys64/mingw64/bin"; do
  if [ -x "$d/g++.exe" ] && [ -x "$d/mingw32-make.exe" ]; then
    export PATH="$d:$PATH"
    break
  fi
done

BUILD_TOOL=""
if command -v ninja >/dev/null 2>&1; then BUILD_TOOL="ninja"
elif command -v mingw32-make >/dev/null 2>&1; then BUILD_TOOL="mingw"
elif command -v make >/dev/null 2>&1; then BUILD_TOOL="make"
fi

if [ -n "$CMAKE_BIN" ] && [ -n "$BUILD_TOOL" ]; then
  echo "[cmake] $("$CMAKE_BIN" --version | head -1)"
  echo "[cmake] 编译器 $(g++ --version | head -1)"
  echo "[cmake] 构建工具 $BUILD_TOOL"

  NONASCII=0
  if printf '%s' "$ROOT" | LC_ALL=C grep -q '[^ -~]'; then NONASCII=1; fi

  if [ "$NONASCII" = "0" ]; then
    SRC_DIR="$ROOT"
    WORK_DIR="$ROOT"
  else
    # 中文路径 → 暂存到 ASCII 临时目录构建
    WORK_DIR="$(mktemp -d)/bcbuild"
    mkdir -p "$WORK_DIR"
    cp -r core CMakeLists.txt "$WORK_DIR"/
    SRC_DIR="$WORK_DIR"
    echo "[cmake] 检测到非 ASCII 路径，暂存构建目录：$WORK_DIR"
  fi

  GEN=()
  case "$BUILD_TOOL" in
    ninja) GEN=(-G "Ninja") ;;
    mingw) GEN=(-G "MinGW Makefiles") ;;
    make)  GEN=(-G "Unix Makefiles") ;;
  esac

  "$CMAKE_BIN" -S "$SRC_DIR" -B "$WORK_DIR/build" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release
  "$CMAKE_BIN" --build "$WORK_DIR/build" --parallel

  if [ "$WORK_DIR" != "$ROOT" ]; then
    cp "$WORK_DIR"/bin/*.exe "$ROOT"/bin/ 2>/dev/null || cp "$WORK_DIR"/bin/* "$ROOT"/bin/
    rm -rf "$WORK_DIR" 2>/dev/null || true
  fi
  echo "编译完成（cmake）→ bin/"
  exit 0
fi

# ---------- 回退：直接调用 g++ ----------
if [ -n "$CMAKE_BIN" ]; then
  echo "[build] 已检测到 cmake，但缺少 ninja / make / mingw32-make，改用 g++ 直接编译"
fi

CXX="${CXX:-g++}"
STD="${STD:--std=c++14}"
OPT="${OPT:--O2}"
FLAGS="$STD $OPT -Wall -Wextra -I core/include"
LIBS=""
SUFFIX=""
if [ "$(uname -s 2>/dev/null | cut -c1-5)" = "MINGW" ] || [ "$OS" = "Windows_NT" ]; then
  LIBS="-ladvapi32 -static -static-libgcc -static-libstdc++"
  SUFFIX=".exe"
fi

SRC="core/src/crypto.cpp core/src/json.cpp core/src/chain.cpp"

echo "[1/3] 编译共识引擎 chain-engine$SUFFIX"
$CXX $FLAGS $SRC core/src/engine.cpp -o "bin/chain-engine$SUFFIX" $LIBS

echo "[2/3] 编译运维命令行 chainctl$SUFFIX"
$CXX $FLAGS $SRC core/cli/chainctl.cpp -o "bin/chainctl$SUFFIX" $LIBS

echo "[3/3] 编译密码学自检程序"
$CXX $FLAGS $SRC core/tests/test_crypto.cpp -o "bin/test_crypto$SUFFIX" $LIBS

echo "编译完成（g++）→ bin/"
