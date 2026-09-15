#!/bin/sh
# Regenerates the test fixtures under test/. Needs ffmpeg (with the EXR
# encoder) and the OpenEXR command line tools; mkexr is built from source.
#
# Runs from Git Bash on Windows too. There the defaults below look for mkexr
# and exrmaketiled in the vcpkg build tree, and use Arial for the burnt-in
# frame numbers. Any of them can be overridden:
#
#   MKEXR=path/to/mkexr  EXRMAKETILED=path/to/exrmaketiled  FONT=path/to.ttf
set -e
cd "$(dirname "$0")/.."

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) on_windows=1 ;;
    *)                    on_windows=0 ;;
esac

# ---- tools ----------------------------------------------------------------

if [ -z "$MKEXR" ]; then
    if [ -x build/windows/Release/mkexr.exe ]; then
        MKEXR=build/windows/Release/mkexr.exe
    elif [ -x build/windows/Debug/mkexr.exe ]; then
        MKEXR=build/windows/Debug/mkexr.exe
    elif [ "$on_windows" = 0 ] && command -v cc >/dev/null 2>&1; then
        echo "building mkexr"
        mkdir -p build
        cc -O2 -Wall -o build/mkexr tools/mkexr.c
        MKEXR=build/mkexr
    else
        echo "mkexr not found: build it (cmake --build ... --target mkexr) or set MKEXR" >&2
        exit 1
    fi
fi

if [ -z "$EXRMAKETILED" ]; then
    if command -v exrmaketiled >/dev/null 2>&1; then
        EXRMAKETILED=exrmaketiled
    elif [ -x build/windows/vcpkg_installed/x64-windows/tools/openexr/exrmaketiled.exe ]; then
        EXRMAKETILED=build/windows/vcpkg_installed/x64-windows/tools/openexr/exrmaketiled.exe
    else
        echo "exrmaketiled not found: install the OpenEXR tools (vcpkg feature 'fixtures') or set EXRMAKETILED" >&2
        exit 1
    fi
fi

if [ -z "$FONT" ]; then
    if [ "$on_windows" = 1 ]; then
        FONT=C:/Windows/Fonts/arial.ttf
    else
        FONT=/usr/share/fonts/liberation/LiberationSans-Regular.ttf
        [ -f "$FONT" ] || FONT=$(fc-match "DejaVu Sans" -f "%{file}")
    fi
fi
if [ ! -f "$FONT" ]; then
    echo "font not found: '$FONT' (set FONT to a .ttf file)" >&2
    exit 1
fi
# ffmpeg's filter syntax treats ':' as an option separator and '\' as an
# escape, so the path is given with forward slashes, the drive letter's colon
# escaped, and the whole value quoted.
FONT_ARG="'$(printf '%s' "$FONT" | sed -e 's|\\|/|g' -e 's/:/\\:/g')'"

echo "main sequence (96 frames, half, zip16, frame number burnt in)"
rm -rf test/seq_a && mkdir -p test/seq_a
ffmpeg -y -v error -f lavfi -i "testsrc2=size=1280x720:rate=24" -frames:v 96 \
  -vf "drawtext=fontfile=$FONT_ARG:text='%{eif\:n+1001\:d}':fontsize=140:fontcolor=white:borderw=6:bordercolor=black:x=(w-tw)/2:y=h-th-60" \
  -pix_fmt gbrpf32le -c:v exr -format half -compression zip16 \
  -start_number 1001 'test/seq_a/beauty.%04d.exr'

echo "window and channel edge cases"
mkdir -p test/edge
"$MKEXR" test/edge/plain.exr    64 48
"$MKEXR" test/edge/overscan.exr 64 48 -10 -8 73 55
"$MKEXR" test/edge/crop.exr     64 48  16 12 47 35
"$MKEXR" test/edge/mono.exr     64 48 y
"$MKEXR" test/edge/rgba.exr     64 48 rgba

echo "tiled and mipmapped"
"$EXRMAKETILED"    test/seq_a/beauty.1001.exr test/edge/tiled.exr
"$EXRMAKETILED" -m test/seq_a/beauty.1001.exr test/edge/mipmap.exr

echo "other pixel types and compressions"
ffmpeg -y -v error -f lavfi -i "testsrc2=size=160x120:rate=1" -frames:v 1 \
  -pix_fmt gbrapf32le -c:v exr -format float -compression none test/edge/float_none.exr
ffmpeg -y -v error -f lavfi -i "testsrc2=size=160x120:rate=1" -frames:v 1 \
  -pix_fmt gbrpf32le  -c:v exr -format half  -compression rle  test/edge/half_rle.exr
ffmpeg -y -v error -f lavfi -i "testsrc2=size=160x120:rate=1" -frames:v 1 \
  -pix_fmt grayf32le  -c:v exr -format half  -compression zip1 test/edge/gray_zip1.exr

echo "damaged files"
head -c 2000 test/edge/plain.exr > test/edge/truncated.exr
head -c 4000 /dev/urandom        > test/edge/garbage.exr
: > test/edge/empty.exr

echo "sequence discovery fixtures"
rm -rf test/sparse test/mixed && mkdir -p test/sparse test/mixed
for n in 1 2 3 7 8 20 100; do
  cp test/edge/plain.exr "test/sparse/shot_$(printf %04d "$n").exr"
done
cp test/edge/plain.exr test/mixed/single.exr
for n in 1 2 3 4 5; do cp test/edge/plain.exr "test/mixed/main.$(printf %03d "$n").exr"; done
for n in 1 2;         do cp test/edge/plain.exr "test/mixed/other.$(printf %03d "$n").exr"; done

# Non-ASCII directory and file names, which must survive the trip through the
# platform's file APIs as UTF-8.
rm -rf test/unicodé && mkdir -p test/unicodé
for n in 1 2 3; do cp test/edge/plain.exr "test/unicodé/ünï.$(printf %03d "$n").exr"; done

echo "done"
