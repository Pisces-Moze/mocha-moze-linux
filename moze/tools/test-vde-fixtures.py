#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run explicit stateless hardware decoding and compare synthetic I420 frames.

The fixture directory contains manifest.json, NAME.h264 and NAME.yuv. The
manifest records width, height, frame count and software reference SHA256s.
No decodebin or software decoder fallback is allowed in the tested pipeline.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixtures', type=Path)
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--decoder', default='v4l2slh264dec')
    parser.add_argument('--capture-format', choices=['I420', 'YV12'], default='I420')
    args = parser.parse_args()
    if not re.fullmatch(r'v4l2slh264(?:device[0-9]+)?dec', args.decoder):
        parser.error('An explicit GStreamer stateless V4L2 H264 decoder is required')
    args.results.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, GST_DEBUG='v4l2codecs:5,v4l2codecs-h264dec:5')
    with (args.results / 'factory.log').open('wb') as log:
        subprocess.run(['gst-inspect-1.0', args.decoder], stdout=log,
                       stderr=log, env=env, check=True, timeout=15)
    records = json.loads((args.fixtures / 'manifest.json').read_text())
    results = []
    for record in records:
        name = record['name']
        if not re.fullmatch(r'[a-z0-9-]+', name):
            raise ValueError('Invalid fixture name')
        video, reference = (args.fixtures / (name + suffix)
                            for suffix in ('.h264', '.yuv'))
        assert digest(video) == record['bitstream_sha256']
        assert digest(reference) == record['raw_sha256']
        output = args.results / (name + '.yuv')
        command = ['gst-launch-1.0', '-e', 'filesrc', 'location=' + str(video),
                   '!', 'h264parse', '!', 'video/x-h264,alignment=au', '!',
                   args.decoder, '!', 'video/x-raw,format=' + args.capture_format,
                   '!', 'videoconvert', '!',
                   'video/x-raw,format=YV12', '!', 'videoconvert', '!',
                   'video/x-raw,format=I420', '!', 'filesink',
                   'location=' + str(output)]
        # A driver I420 buffer may retain a 128-byte pitch in GstVideoMeta.
        # Force lossless U/V plane swaps so filesink gets newly allocated
        # canonical I420 instead of passthrough buffers with hardware padding.
        start = time.monotonic()
        with (args.results / (name + '.log')).open('wb') as log:
            run = subprocess.run(command, stdout=log, stderr=log,
                                 env=env, timeout=30)
        data = output.read_bytes() if output.exists() else b''
        frame_size = record['width'] * record['height'] * 3 // 2
        hashes = [hashlib.sha256(data[p:p + frame_size]).hexdigest()
                  for p in range(0, len(data), frame_size)]
        passed = (run.returncode == 0 and len(data) == record['frames'] * frame_size
                  and hashes == record['frame_sha256'])
        results.append(dict(name=name, passed=passed, returncode=run.returncode,
                            capture_format=args.capture_format,
                            seconds=time.monotonic() - start, bytes=len(data),
                            frame_sha256=hashes))
        (args.results / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        print(name, 'EXACT_FRAME_MATCH' if passed else 'FAILED', flush=True)
        if not passed:
            raise SystemExit('Hardware output did not match the software reference')
    print('ALL_CAVLC_FIXTURES_MATCHED_EXPLICIT_HARDWARE_PIPELINE')


if __name__ == '__main__':
    main()
