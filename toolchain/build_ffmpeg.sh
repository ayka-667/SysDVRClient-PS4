#!/bin/bash
# Builds a minimal FFmpeg (H.264 decoder + swscale) for the PS4 (OpenOrbis).
# Run under Linux or WSL. Output: $PREFIX/{include,lib}
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-$HOME/ps4build}"
FFVER="${FFVER:-7.1.1}"
PREFIX="${PREFIX:-$WORK/ffmpeg-ps4}"
export OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$WORK/oo}"

mkdir -p "$WORK/bin"
cp "$HERE/ps4-cc" "$WORK/bin/ps4-cc"
sed -i 's/\r$//' "$WORK/bin/ps4-cc"
chmod +x "$WORK/bin/ps4-cc"

cd "$WORK"
if [ ! -d "ffmpeg-$FFVER" ]; then
  curl -sL "https://ffmpeg.org/releases/ffmpeg-$FFVER.tar.xz" -o ffmpeg.tar.xz
  tar xf ffmpeg.tar.xz
fi
cd "ffmpeg-$FFVER"

./configure \
  --prefix="$PREFIX" \
  --enable-cross-compile --arch=x86_64 --cpu=btver2 --target-os=none \
  --cc="$WORK/bin/ps4-cc" --ld="$WORK/bin/ps4-cc" \
  --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm --strip=llvm-strip \
  --x86asmexe=nasm \
  --enable-static --disable-shared --enable-pic \
  --disable-everything --disable-autodetect \
  --disable-programs --disable-doc --disable-debug \
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample \
  --disable-network --disable-iconv \
  --enable-pthreads \
  --enable-decoder=h264 --enable-parser=h264 \
  --enable-swscale \
  --extra-cflags="-O3"

# The PS4 kernel is FreeBSD-based: disable Linux/BSD-specific CPU count probes
# (they would issue syscalls that don't exist / don't behave on the PS4).
sed -i -e 's/#define HAVE_SYSCTL 1/#define HAVE_SYSCTL 0/' \
       -e 's/#define HAVE_SCHED_GETAFFINITY 1/#define HAVE_SCHED_GETAFFINITY 0/' \
       -e 's/#define HAVE_GETAUXVAL 1/#define HAVE_GETAUXVAL 0/' \
       -e 's/#define HAVE_SYSCONF 1/#define HAVE_SYSCONF 0/' config.h

make -j"$(nproc)"
make install

# FFmpeg 7.1 bug: h2645_sei.o references aom_film_grain.o which is only built with HEVC enabled
make libavcodec/aom_film_grain.o
llvm-ar rcs "$PREFIX/lib/libavcodec.a" libavcodec/aom_film_grain.o
echo "PS4 FFmpeg installed to $PREFIX"
