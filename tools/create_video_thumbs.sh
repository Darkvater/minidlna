#!/bin/sh

# Generate thumbnails for video files shared with MiniDLNA.
#
# As for now, directories with video content should be marked as "video"
# to make minidlnad use images for thumbnails. Do it with the following
# setting in minidlna.conf:
#
#   media_dir=V,/mnt/sda1/multimedia/video
#
# Inspired by https://groups.google.com/forum/#!topic/alt-f/8BkQQDyb2j0

MEDIA_DIR=/mnt/sda1/multimedia

# Some thumbnails should have .cover suffix
find $MEDIA_DIR \( -name \*.mkv -o -name \*.mp4 \) | while read ln; do
  OUTFILE="$(dirname "$ln")/$(basename "$ln" .avi).cover.jpg"
  if [ ! -f "$OUTFILE" ]; then
    ffmpegthumbnailer -s0 -q9 -i "$ln" -o "$OUTFILE"
  fi
done

# AVI thumbnails should have no additional suffix
find $MEDIA_DIR \( -name \*.avi \) | while read ln; do
  OUTFILE="$(dirname "$ln")/$(basename "$ln" .avi).jpg"
  if [ ! -f "$OUTFILE" ]; then
    ffmpegthumbnailer -s0 -q9 -i "$ln" -o "$OUTFILE"
  fi
done
