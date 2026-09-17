#!/bin/sh
# Regenerates the video fixtures under test/video. Needs an ffmpeg with
# libx264 and libx265 on PATH (the library build ramplayer links has no
# encoders, so these cannot be made in-tree).
#
# Every fixture is 96 frames of 320x180 at 24 fps. Frame N is a flat colour
#     R = (N * 53) mod 256   G = (N * 97) mod 256   B = (N * 29) mod 256
# so neighbouring frames differ by far more than compression error, and a
# test can tell exactly which frame it was given. Encoded at high quality.
set -e
cd "$(dirname "$0")/.."

command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg not found on PATH" >&2; exit 1; }

mkdir -p test/video

# The pattern: a black source turned into the per-frame colour by geq, which
# evaluates its expressions per pixel with N the frame number.
SRC="color=c=black:s=320x180:r=24"
PATTERN="format=rgb24,geq=r='mod(N*53,256)':g='mod(N*97,256)':b='mod(N*29,256)'"

echo "h264, closed groups of 24, B-frames, tagged BT.709"
ffmpeg -y -v error -f lavfi -i "$SRC" -frames:v 96 \
  -vf "$PATTERN,scale=out_color_matrix=bt709:out_range=tv" -pix_fmt yuv420p \
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 \
  -c:v libx264 -preset medium -crf 10 -g 24 -keyint_min 24 -sc_threshold 0 -bf 3 \
  -movflags +faststart test/video/h264_gop24.mp4

echo "h265, 10-bit, groups of 24, B-frames, untagged"
ffmpeg -y -v error -f lavfi -i "$SRC" -frames:v 96 \
  -vf "$PATTERN" -pix_fmt yuv420p10le \
  -c:v libx265 -preset medium -crf 10 -x265-params "keyint=24:min-keyint=24:scenecut=0:bframes=3:log-level=error" \
  -tag:v hvc1 -movflags +faststart test/video/h265_gop24_10bit.mp4

echo "h264, a single keyframe for the whole file"
ffmpeg -y -v error -f lavfi -i "$SRC" -frames:v 96 \
  -vf "$PATTERN" -pix_fmt yuv420p \
  -c:v libx264 -preset medium -crf 10 -g 1000 -keyint_min 1000 -sc_threshold 0 -bf 3 \
  -movflags +faststart test/video/h264_onegop.mp4

echo "h264, open groups of 24"
ffmpeg -y -v error -f lavfi -i "$SRC" -frames:v 96 \
  -vf "$PATTERN" -pix_fmt yuv420p \
  -c:v libx264 -preset medium -crf 10 -g 24 -keyint_min 24 -sc_threshold 0 -bf 3 \
  -x264-params open-gop=1 -movflags +faststart test/video/h264_opengop.mp4

echo "done"
