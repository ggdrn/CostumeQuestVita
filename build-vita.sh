#!/bin/bash
# Builds the VPK on macOS through the OrbStack Linux machine "vitadev",
# which has the softfp VitaSDK in ~/vitasdk-softfp (no macOS build exists).
#
#   ./build-vita.sh            # Debug (logs enabled) -> build/costume_quest_debug.vpk
#   ./build-vita.sh Release    # for playing        -> build/costume_quest.vpk
#   ./build-vita.sh Release fmod  # + FMOD debug libs (audio issues) -> build/costume_quest_fmod.vpk
#
# LiveArea art comes from the game's APK when one is found (CQ_APK, or
# CostumeQuestPowerVR.apk next to this folder); otherwise placeholders are used.

set -e

BUILD_TYPE="${1:-Debug}"
VARIANT="${2:-}"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
APK="${CQ_APK:-$SRC_DIR/../CostumeQuestPowerVR.apk}"

orb -m vitadev bash -s -- "$SRC_DIR" "$BUILD_TYPE" "$VARIANT" "$APK" <<'EOF'
set -e
SRC_DIR="$1"
BUILD_TYPE="$2"
VARIANT="$3"
APK="$4"
EXTRA=""
BUILD_DIR=~/cq-build-$BUILD_TYPE
if [ "$VARIANT" = "fmod" ]; then
  EXTRA="-DFMOD_DEBUG=ON"
  BUILD_DIR=~/cq-build-$BUILD_TYPE-fmod
fi
export VITASDK=~/vitasdk-softfp
export PATH=$VITASDK/bin:$PATH

# vita.cmake doesn't quote VPK file paths, and the project path has spaces
ln -sfn "$SRC_DIR" ~/cq-src

# LiveArea images from the user's APK (build/ is not committed)
if [ -f "$APK" ]; then
  python3 ~/cq-src/extras/scripts/livearea_from_apk.py "$APK" ~/cq-src/build/livearea
else
  echo "No APK at $APK: placeholder LiveArea images"
fi

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
