# transcode: convert videos on the hardware encoder, and audio

`ffmpeg` in the guest works, but it encodes video in software, and that
software is emulated: on a phone, converting to H.265 can run at well under
1% of real time. `transcode` hands the whole job to the video encoder built
into your iPhone or iPad instead, the same one the Camera and Photos apps
use. It runs at full speed whatever your root's architecture.

It converts audio too, with the system's own AAC, ALAC and PCM encoders: see
[Audio](#audio) below.

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

## Without a terminal

In Workspace, the **Media Converter** applet does the same job with buttons,
for videos and audio files, and also takes videos straight from Photos, puts
the result back there, and deletes the original for you when you ask. See
[workspace.md](workspace.md#media-converter).

## Options

| Option | What it does |
|---|---|
| `-c hevc` / `-c h264` | the video codec. HEVC (the default) gives the same quality in about two thirds of the space; H.264 plays on more old devices |
| `-c aac` / `-c alac` / `-c pcm` | the codec of an audio file (see [Audio](#audio)) |
| `-s LINES` | shrink so the shorter side is at most LINES: `-s 1080`, `-s 720`. Portrait and landscape videos are both handled, and nothing is made bigger |
| `-b RATE` | the video bitrate, such as `-b 2M` or `-b 1500k`. Without it the rate is chosen from the size and frame rate and capped at the source's. That is a target, not a promise: a video that was already small can still come out larger, and `transcode` says so when it does |
| `-a RATE` | the AAC bitrate, default 128k. A video's audio uses it only when it has to be re-encoded (see below); an audio file is re-encoded at it whenever it is given. A rate the encoder does not take for that sample rate and channel count becomes the nearest one below that it does |
| `-n` | drop the audio |
| `-y` | overwrite the output if it exists |
| `-q` | no progress line or summary |

An output name ending in `.mov` makes a QuickTime file; `.m4a`, `.caf`,
`.wav` and `.aif` make an audio file; anything else makes MPEG-4.

## Audio

Name the output after the audio file you want:

```sh
/AOK/native/transcode song.flac song.m4a          # AAC, 128 kbit/s
/AOK/native/transcode -c alac song.flac song.m4a  # ALAC, lossless
/AOK/native/transcode song.mp3 song.wav           # PCM
/AOK/native/transcode IMG_1234.MOV sound.m4a      # a video's sound alone
```

| Output | Holds |
|---|---|
| `.m4a` | AAC (the default) or ALAC (`-c alac`) |
| `.caf` | AAC, ALAC or PCM |
| `.wav`, `.aif` | PCM |
| `.mp4`, `.mov` | an audio-only file, when the input has no video |

- **Input**: whatever iOS decodes: MP3, AAC, ALAC, FLAC, WAV, AIFF and CAF
  files, and the sound of a video. Ogg and Opus files are not among them.
- **AAC and ALAC are copied, not re-encoded**, when the input already has
  them and the output takes them, so `transcode clip.mov clip.m4a` is
  quick and loses nothing. Give `-a` to re-encode AAC at another rate.
- **What is kept**: the sample rate (AAC goes no higher than 48 kHz, so 96 kHz
  becomes 48), mono or stereo (more channels are mixed down to stereo), and
  24 bits for ALAC and PCM from a source that has them. Title, artist and
  album, and a video's date and place, go into an `.m4a`; an `.mp4` keeps the
  title only, and a WAV or AIFF file none.
- **No MP3, FLAC or Opus output**: iOS has no encoder for them.
  `transcode` says so, and ffmpeg in the guest can still make them.

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
- **Audio, as it is where possible.** In a `.mov`, the audio is copied
  unchanged. In an MPEG-4 file only AAC is copied; anything else (the
  uncompressed audio of a Live Photo, the spatial audio newer iPhones
  record) is re-encoded to stereo AAC at the `-a` rate, so the file plays
  everywhere.

## Limits

- **The input has to be an ordinary file** on your root, in your home
  directory, or in a folder from the Files app. iOS reads it directly, so a
  pipe, a file on tmpfs, or a file in `/proc` will not do: copy it somewhere
  ordinary first. The output can go anywhere.
- **The output is built in the app's own temporary space** and then copied
  to where you asked, so you need free space for the output (not for a
  second copy of the input).
- **Keep AOK-iSH in the foreground** while it runs. iOS suspends apps in
  the background, and a conversion cannot carry on while the app is
  suspended.
- **Ctrl-C cancels cleanly**: the encoder stops and the partial file is
  removed.
- It is not ffmpeg: no filters, no other codecs, no streams other than the
  first video and the first audio track. For anything else, use ffmpeg and
  expect it to be slow.
