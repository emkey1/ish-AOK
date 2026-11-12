# transcode: convert videos on the hardware encoder

`ffmpeg` in the guest works, but it encodes video in software, and that
software is emulated: on a phone, converting to H.265 can run at well under
1% of real time. `transcode` hands the whole job to the video encoder built
into your iPhone or iPad instead, the same one the Camera and Photos apps
use. It runs at full speed whatever your root's architecture.

It is a [native program](native-programs.md), so it is always there:

```sh
/AOK/native/transcode IMG_1234.MOV small.mp4
```

That re-encodes to HEVC (H.265) at the same size, at a bitrate chosen from
the picture size, and copies the audio across. When it finishes it prints
what it did (this one was a screen recording, with `-c h264`):

```
small.mp4: 38.3 MB -> 13.6 MB (36%) in 28.1 s; H.264 1206x2622, 1.8 Mbit/s (auto), no audio
```

## Options

| Option | What it does |
|---|---|
| `-c hevc` / `-c h264` | the codec. HEVC (the default) gives the same quality in about two thirds of the space; H.264 plays on more old devices |
| `-s LINES` | shrink so the shorter side is at most LINES: `-s 1080`, `-s 720`. Portrait and landscape videos are both handled, and nothing is made bigger |
| `-b RATE` | the video bitrate, such as `-b 2M` or `-b 1500k`. Without it the rate is chosen from the size and frame rate and capped at the source's. That is a target, not a promise: a video that was already small can still come out larger, and `transcode` says so when it does |
| `-a RATE` | the audio bitrate, used only when the audio has to be re-encoded (see below). Default 128k |
| `-n` | drop the audio |
| `-y` | overwrite the output if it exists |
| `-q` | no progress line or summary |

An output name ending in `.mov` makes a QuickTime file; anything else makes
MPEG-4.

## Freeing space: a recipe

To shrink the videos in a folder you have opened from the Files app (see
[files-app-integration.md](files-app-integration.md)) to 1080p HEVC:

```sh
for f in *.MOV; do /AOK/native/transcode -s 1080 "$f" "${f%.*}-small.mp4"; done
```

Check a few results before deleting any originals.

## What it keeps

- **Orientation, creation date and location.** The rotation and the
  original's metadata are copied, so the result still sorts and maps where
  the original did.
- **HDR, when the output is HEVC.** An HDR video (what recent iPhones record
  by default) stays HDR, in 10 bits. H.264 has no HDR, so with `-c h264` the
  picture is tone-mapped to ordinary SDR.
- **Audio, as it is where possible.** AAC and most other audio are copied
  unchanged. Audio that an MPEG-4 file cannot carry as it is (newer iPhones
  record spatial audio) is re-encoded to stereo AAC at the `-a` rate.

## Limits

- **The input has to be an ordinary file** on your root, in your home
  directory, or in a folder from the Files app. iOS reads it directly, so a
  pipe, a file on tmpfs, or a file in `/proc` will not do: copy it somewhere
  ordinary first. The output can go anywhere.
- **The output is built in the app's own temporary space** and then copied
  to where you asked, so you need free space for the output (not for a
  second copy of the input).
- **Keep iSH-AOK in the foreground** while it runs. iOS suspends apps in
  the background, and a conversion cannot carry on while the app is
  suspended.
- **Ctrl-C cancels cleanly**: the encoder stops and the partial file is
  removed.
- It is not ffmpeg: no filters, no other codecs, no streams other than the
  first video and the first audio track. For anything else, use ffmpeg and
  expect it to be slow.
