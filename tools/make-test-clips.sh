#!/bin/sh
# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
# Makes the test clips (no sound): an HDR10 one with known metadata, an SDR one, a VP9 one for Firefox, and
# an HLG one and a Display P3 one.
# Existing clips are kept; nothing is overwritten.
# HDR10: PQ, BT.2020, mastering display P3-D65 0.0001-1000 cd/m², MaxCLL 1000, MaxFALL 400.
# Usage: make-test-clips.sh [output folder, default ~/RiftHomeVR/test-media]
set -eu
OUT=${1:-$HOME/RiftHomeVR/test-media}
mkdir -p "$OUT"
FF="nice -n 10 ffmpeg -nostdin -hide_banner -loglevel error"
SRC="testsrc2=size=1920x1080:rate=24:duration=30"

if [ ! -e "$OUT/hdr10-pq-bt2020-1000nits.mkv" ]; then
	$FF -f lavfi -i "$SRC" -vf format=yuv420p10le -c:v libx265 -preset ultrafast \
		-x265-params "log-level=error:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:hdr10=1:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400" \
		-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc "$OUT/hdr10-pq-bt2020-1000nits.mkv"
fi
if [ ! -e "$OUT/sdr-bt709.mkv" ]; then
	$FF -f lavfi -i "$SRC" -vf format=yuv420p -c:v libx264 -preset ultrafast \
		-color_primaries bt709 -color_trc bt709 -colorspace bt709 "$OUT/sdr-bt709.mkv"
fi
if [ ! -e "$OUT/sdr-bt709-vp9.webm" ]; then
	$FF -f lavfi -i "$SRC" -vf format=yuv420p -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 2M \
		-color_primaries bt709 -color_trc bt709 -colorspace bt709 "$OUT/sdr-bt709-vp9.webm"
fi
# Colours Rift Home cannot be told (wlroots 0.20 offers no HLG and no Display P3): mpv converts them.
if [ ! -e "$OUT/hlg-bt2020.mkv" ]; then
	$FF -f lavfi -i "$SRC" -vf format=yuv420p10le -c:v libx265 -preset ultrafast \
		-x265-params "log-level=error:colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc" \
		-color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc "$OUT/hlg-bt2020.mkv"
fi
if [ ! -e "$OUT/sdr-p3-d65.mkv" ]; then
	$FF -f lavfi -i "$SRC" -vf format=yuv420p -c:v libx264 -preset ultrafast \
		-x264-params "colorprim=smpte432:transfer=iec61966-2-1:colormatrix=bt709" \
		-color_primaries smpte432 -color_trc iec61966-2-1 -colorspace bt709 "$OUT/sdr-p3-d65.mkv"
fi
ls -l "$OUT"
