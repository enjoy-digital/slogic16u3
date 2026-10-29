#!/bin/sh
# Deliverables for doc/images from the renders of render-video.sh: README hero (1600px), high resolution
# still, README snapshots and the web-friendly video (hero still as poster frame).
set -e
cd "$(dirname "$0")"
OUT=../images
mkdir -p $OUT
convert slogic_16u3-hero.png -resize 1600x -quality 92 $OUT/slogic_16u3.jpg
convert slogic_16u3-hero.png               -quality 90 $OUT/slogic_16u3-hires.jpg
for s in inside architecture capture software update opensource; do
    convert slogic_16u3-$s.png -resize 1600x -quality 90 $OUT/slogic_16u3-$s.jpg
done
ffmpeg -y -hide_banner -loglevel error -i slogic_16u3.mp4 -loop 1 -framerate 30 -t 0.0667 -i slogic_16u3-hero.png \
    -filter_complex "[1:v]scale=1920:1080,format=yuv420p,setsar=1[p];[0:v]format=yuv420p,setsar=1[v];[p][v]concat=n=2:v=1:a=0[o];[0:a]adelay=67:all=1[a]" \
    -map "[o]" -map "[a]" -c:v libx264 -preset slow -crf 26 -c:a aac -b:a 160k \
    -pix_fmt yuv420p -movflags +faststart $OUT/slogic_16u3.mp4
ls -l $OUT/slogic_16u3*
