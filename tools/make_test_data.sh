#!/bin/sh
# Regenerates the test fixtures under test/. Needs ffmpeg (with the EXR
# encoder) and the OpenEXR command line tools; mkexr is built from source.
set -e
cd "$(dirname "$0")/.."

FONT=${FONT:-/usr/share/fonts/liberation/LiberationSans-Regular.ttf}
[ -f "$FONT" ] || FONT=$(fc-match "DejaVu Sans" -f "%{file}")

echo "building mkexr"
cc -O2 -Wall -o build/mkexr tools/mkexr.c

echo "main sequence (96 frames, half, zip16, frame number burnt in)"
rm -rf test/seq_a && mkdir -p test/seq_a
ffmpeg -y -v error -f lavfi -i "testsrc2=size=1280x720:rate=24" -frames:v 96 \
  -vf "drawtext=fontfile=$FONT:text='%{eif\:n+1001\:d}':fontsize=140:fontcolor=white:borderw=6:bordercolor=black:x=(w-tw)/2:y=h-th-60" \
  -pix_fmt gbrpf32le -c:v exr -format half -compression zip16 \
  -start_number 1001 'test/seq_a/beauty.%04d.exr'

echo "window and channel edge cases"
mkdir -p test/edge
build/mkexr test/edge/plain.exr    64 48
build/mkexr test/edge/overscan.exr 64 48 -10 -8 73 55
build/mkexr test/edge/crop.exr     64 48  16 12 47 35
build/mkexr test/edge/mono.exr     64 48 y
build/mkexr test/edge/rgba.exr     64 48 rgba

echo "tiled and mipmapped"
exrmaketiled    test/seq_a/beauty.1001.exr test/edge/tiled.exr
exrmaketiled -m test/seq_a/beauty.1001.exr test/edge/mipmap.exr

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

echo "done"
