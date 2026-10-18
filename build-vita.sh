#!/bin/bash
# Builds the VPK on macOS through the OrbStack Linux machine "vitadev",
# which has the softfp VitaSDK in ~/vitasdk-softfp (no macOS build exists).
#
#   ./build-vita.sh            # Debug (logs enabled) -> build/costume_quest_debug.vpk
#   ./build-vita.sh Release    # for playing        -> build/costume_quest.vpk
#   ./build-vita.sh Release fmod  # + FMOD debug libs (audio issues) -> build/costume_quest_fmod.vpk
#
# On the feat_controles branch the VPK is build/costume_quest_controls.vpk.

set -e

BUILD_TYPE="${1:-Debug}"
VARIANT="${2:-}"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$VARIANT" ] && [ "$(git -C "$SRC_DIR" branch --show-current 2>/dev/null)" = "feat_controles" ]; then
  VARIANT=controls
fi

orb -m vitadev bash -s -- "$SRC_DIR" "$BUILD_TYPE" "$VARIANT" <<'EOF'
set -e
SRC_DIR="$1"
BUILD_TYPE="$2"
VARIANT="$3"
EXTRA=""
BUILD_DIR=~/cq-build-$BUILD_TYPE
if [ "$VARIANT" = "fmod" ]; then
  EXTRA="-DFMOD_DEBUG=ON"
  BUILD_DIR=~/cq-build-$BUILD_TYPE-fmod
elif [ -n "$VARIANT" ]; then
  BUILD_DIR=~/cq-build-$BUILD_TYPE-$VARIANT
fi
export VITASDK=~/vitasdk-softfp
export PATH=$VITASDK/bin:$PATH

# vita.cmake doesn't quote VPK file paths, and the project path has spaces
ln -sfn "$SRC_DIR" ~/cq-src

# vitaGL's Makefile expects its header to be installed in the SDK
cp ~/cq-src/lib/vitagl/source/vitaGL.h $VITASDK/arm-vita-eabi/include/

cmake -S ~/cq-src -B $BUILD_DIR -DCMAKE_BUILD_TYPE=$BUILD_TYPE $EXTRA
cmake --build $BUILD_DIR -j"$(nproc)"

mkdir -p ~/cq-src/build
if [ "$BUILD_TYPE" = "Release" ]; then OUT=costume_quest; else OUT=costume_quest_debug; fi
[ -n "$VARIANT" ] && OUT=${OUT}_$VARIANT
OUT=$OUT.vpk
cp $BUILD_DIR/costume_quest.vpk ~/cq-src/build/$OUT
echo "VPK: $SRC_DIR/build/$OUT"
EOF
