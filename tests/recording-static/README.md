# Static recording dependency smoke

After the isolated static FFmpeg dependency prefix is installed, run
`./run.ps1`. The defaults use the compact build's FFmpeg prefix and native
UCRT64 toolchain. `-FFmpegPrefix`, `-BuildDirectory`, `-Compiler`, `-PkgConfig`,
and `-Objdump` can select another matching installation.

This is a ROM-free check of the dependency recipe's recording capabilities,
not a replacement for an mGBA recording test. It links the actual FFmpeg
archives through their isolated `pkg-config --static` metadata, uses static
compiler/C++ runtimes, rejects non-Windows-system PE DLL imports, and runs
without dependency/compiler directories on PATH. It does not link Qt or
launch the emulator, a game, a GUI, or a decoder.

Required capabilities include x264/x264 RGB, AAC, FFV1, PNG, GIF, Opus,
VP8/VP9, MP3, WebP/WebP animation, x265 and FLAC encoders; palettegen,
paletteuse, scale, split, buffer and buffersink filters; and MP4, WebM, GIF,
Matroska, AVI and WAV muxers. Encoder lookup proves registration, not that
every possible format/codec combination or hardware device works.

By default, four synthetic 16-by-16 RGB frames pass through the same basic
buffer/split/palettegen/paletteuse/buffersink arrangement used for paletted
recording, then the GIF encoder and muxer write a small output. The smoke
checks frame/packet counts and GIF signature/trailer; it does not decode the
file because the source recipe intentionally disables decoders and demuxers.
`-SkipSyntheticGif` runs only the capability/import checks.

The runner retains its fresh fixture directory, executable, link response,
PE import report, and synthetic GIF under the selected build directory. It
does not alter existing recordings, application settings, saves, source
recipes, or application build flags. Passing it does not prove the actual
frontend's recording UI or audio/video synchronization is correct.
