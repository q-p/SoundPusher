# Sound Pusher — real-time encoded audio output for OS X
Provides a virtual 5.1-channel audio output device whose contents are real-time encoded to `AC3` and sent as digital `SPDIF` stream to a real audio device output.

**WARNING:** Please turn down the speaker / headphone volume before running this application, as any component misinterpreting a compressed audio stream as "normal" audio signals may result in unexpectedly loud noise.

## Requirements
The application has been developed on OS X 10.11 "El Capitan", but *should* work on OS X 10.10 "Yosemite" as well. It requires a compatible audio device capable of outputting compressed `AC3` audio formats (format IDs `ac-3`, `cac3`, `IAC3`, `iac3`). All testing was done with `cac3` (aka `kAudioFormat60958AC3`).

## Usage
There are two components required to make this work:

1. `LoopbackAudio` — A user-space CoreAudio driver (aka `AudioServerPlugin`) that provides a 5.1 output stream whose contents are mirrored ("looped back") to its own input stream. This allows applications to process the audio output of the system (if it is sent to the `LoopbackAudio` device). `LoopbackAudio.driver` must be installed (copied) into the `/Library/Audio/Plug-Ins/HAL` directory.
2. `SoundPusher` — An application that continuously reads audio from the `LoopbackAudio` device, encodes it into a compressed format, and then sends that compressed stream to a (hopefully) real sound device's digital output stream. This is controlled through its menu bar extra.

Once the driver is installed and `SoundPusher` running, you should be able to select supported digital output devices which will forward any sound output sent to the `Loopback Audio` device to the designated digital output. `SoundPusher` will set the default output device to `Loopback Audio` if it was previously set to the device used for digital output.

## Contact & Support
Please report any issues on [GitHub](https://github.com/q-p/SoundPusher).

## Dependencies
### FFmpeg — libavcodec, libavformat, libswresample
Here's a script that builds a cut-down version of [FFmpeg](http://www.ffmpeg.org) that includes all that's required for SoundPusher:
```sh
#!/bin/sh
./configure --prefix=$FFMPEG_HOME --cc=clang --disable-static --enable-shared --disable-all --disable-doc --disable-everything --disable-xlib --disable-pthreads --disable-iconv --disable-securetransport --disable-audiotoolbox --disable-videotoolbox --disable-appkit --disable-avfoundation --disable-coreimage --disable-bzlib --disable-zlib --enable-avutil --enable-avcodec --enable-avformat --enable-swresample --enable-encoder=ac3 --enable-muxer=spdif
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

## Acknowledgements
This software is built on the experience of others:
- `LoopbackAudio` is based on the `NullAudio` user-space driver provided by Apple as example code.
- [TPCircularBuffer](https://github.com/michaeltyson/TPCircularBuffer/) by Michael Tyson is used to move audio and packet data around.
- [FFmpeg](http://www.ffmpeg.org) is used to encode to compressed formats and then mux those packets into an `SPDIF` compliant bit-stream.
