# Sound Pusher — real-time encoded audio output for OS X
Provides a virtual 5.1-channel audio output device whose contents are real-time encoded to `AC3` and sent as digital `SPDIF` stream to a real audio device output.

**WARNING:** Please turn down the speaker / headphone volume before running this application, as any component misinterpreting a compressed audio stream as "normal" audio signals may result in unexpectedly loud noise.

## Requirements
The application is being developed on OS X 11 "Big Sur", but *should* work back to OS X 10.14 "Mojave" as well. It requires a compatible audio device capable of outputting compressed `AC3` audio formats (format IDs `ac-3`, `cac3`, `IAC3`, `iac3`). All testing was done with `cac3` (aka `kAudioFormat60958AC3`).

## Usage
There are two components required to make this work:

1. `SoundPusherAudio` — A user-space (sand-boxed) CoreAudio driver (aka `AudioServerPlugin`) that provides a 5.1 channel output stream whose contents are mirrored ("looped back") to its own input stream. This allows applications to process the audio output of the system (if it is sent to the `SoundPusher Audio` device). `SoundPusherAudio.driver` must be installed (copied) into the `/Library/Audio/Plug-Ins/HAL` directory. This driver was previously called `Loopback Audio` and you may have to remove the old version (called `LoopbackAudio.driver`) manually. Instead of this, you can also use any other suitable (6 channel, 48kHz) input device, e.g. [Rogue Amoeba's Loopback](https://rogueamoeba.com/loopback/).
2. `SoundPusher` — An application that continuously reads audio from `SoundPusher Audio` (or any other suitable) device, encodes it into a compressed format, and then sends that compressed stream to a (hopefully) real sound device's digital output stream. This is controlled through its menu bar extra.

Once the driver is installed and `SoundPusher` running, you should be able to select the desired combination of supported input and digital output devices (which will forward any sound output from the input device to the designated digital output). `SoundPusher` will set the default output device to the selected input device (e.g. `SoundPusher Audio`) if it was previously set to the device used for digital output.

## Contact & Support
Please report any issues on [GitHub](https://github.com/q-p/SoundPusher).

## Dependencies
### FFmpeg — libavcodec, libavformat, libswresample
Here's a script that builds a cut-down version of [FFmpeg](http://www.ffmpeg.org) that includes all that's required for SoundPusher:
```sh
#!/bin/sh
MACOSX_DEPLOYMENT_TARGET=10.14 export MACOSX_DEPLOYMENT_TARGET
# when compiling for x86_64 on Apple Silicon (aarch64) you probably want to install yasm and add the following:
# --extra-cflags="-target x86_64-apple-macos10.14" --extra-ldflags="-target x86_64-apple-macos10.14" --arch=x86 --x86asmexe=<PATH_TO_YASM>/bin/yasm
./configure --prefix=$FFMPEG_HOME --cc=clang --extra-cflags="-fno-stack-check" --disable-static --enable-shared --disable-all --disable-autodetect --disable-programs --disable-doc --disable-everything --disable-pthreads --disable-network --disable-dct --disable-dwt --disable-lsp --disable-lzo --disable-rdft --disable-faan --disable-pixelutils --enable-avutil --enable-avcodec --enable-avformat --enable-swresample --enable-encoder=ac3 --enable-muxer=spdif
make -j8
make install
# update shared library install names
cd $FFMPEG_HOME/lib
AVUTIL_ID=`otool -D libavutil.dylib | tail -1`
AVCODEC_ID=`otool -D libavcodec.dylib | tail -1`
AVFORMAT_ID=`otool -D libavformat.dylib | tail -1`
SWRESAMPLE_ID=`otool -D libswresample.dylib | tail -1`
for lib in libavutil libavcodec libavformat libswresample
do
    install_name_tool -id @rpath/$lib.dylib -change $AVUTIL_ID @rpath/libavutil.dylib -change $AVCODEC_ID @rpath/libavcodec.dylib -change $AVFORMAT_ID @rpath/libavformat.dylib -change $SWRESAMPLE_ID @rpath/libswresample.dylib $lib.dylib
done
```
If you're building a universal binary (for Intel (x86_64) and Apple Silicon (aarch64)) you probably want to compile FFmpeg twice and then  `lipo --create <inLibA> <inLibB> -output <outLib>` all the resulting libs together.
Note that you should add `FFMPEG_HOME` in Xcode's `Preferences` -> `Locations` -> `Custom Paths` and point it towards the install directory of FFmpeg so that the Xcode project can find it.

## Acknowledgements
This software is built on the experience of others:
- `SoundPusherAudio` (formerly known as `LoopbackAudio`) is based on the `NullAudio` user-space driver provided by Apple as example code.
- [TPCircularBuffer](https://github.com/michaeltyson/TPCircularBuffer/) by Michael Tyson is used to move audio and packet data around.
- [FFmpeg](http://www.ffmpeg.org) is used to encode to compressed formats and then mux those packets into an `SPDIF` compliant bit-stream.
