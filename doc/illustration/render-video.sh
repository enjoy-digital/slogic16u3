#!/bin/sh
# Render the SLogic16U3 promo video (with its soundtrack) + hero/README stills, deterministic, frame by frame.
# Needs: google-chrome, node, ffmpeg, imagemagick, python3 (numpy/scipy for music.py). three.js from jsDelivr.
# Usage: ./render-video.sh [width] [height] [fps] [workers]   (then ./finalize.sh for doc/images)
set -e
W=${1:-1920}; H=${2:-1080}; FPS=${3:-30}; WORKERS=${4:-4}; PORT=8769; DUR=76
cd "$(dirname "$0")"
[ -d node_modules/puppeteer-core ] || PUPPETEER_SKIP_DOWNLOAD=1 npm install --silent
python3 music.py music.wav
ffmpeg -y -loglevel error -i music.wav -c:a libmp3lame -b:a 160k music.mp3
python3 -m http.server $PORT --directory .. >/dev/null 2>&1 & SRV=$!   # doc/: the page loads ../images/
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1
URL=http://localhost:$PORT/illustration/video.html
STEP=$((DUR / WORKERS)); PIDS=""; : > _parts.txt
for i in $(seq 0 $((WORKERS - 1))); do
    T0=$((i * STEP)); T1=$(( i == WORKERS - 1 ? DUR : (i + 1) * STEP ))
    node capture.js "$URL" _part$i.mp4 $W $H $FPS $T0 $T1 > _part$i.log 2>&1 & PIDS="$PIDS $!"
    echo "file '_part$i.mp4'" >> _parts.txt
done
for p in $PIDS; do wait $p; done
ffmpeg -y -loglevel error -f concat -safe 0 -i _parts.txt -i music.wav -map 0:v -map 1:a -c:v copy \
    -c:a aac -b:a 192k -shortest -movflags +faststart slogic_16u3.mp4
rm -f _part*.mp4 _part*.log _parts.txt
node sheet.js "$URL?still" hero 3200 1800 73 && mv hero_00073.png slogic_16u3-hero.png
for snap in inside:12.9 architecture:35.4 capture:41.0 software:49.0 update:55.0 opensource:62.5; do
    node sheet.js "$URL?clean" "_snap" 2400 1350 "${snap#*:}" && mv _snap_*.png "slogic_16u3-${snap%%:*}.png"
done
