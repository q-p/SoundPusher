//
//  SPDIFAudioEncoder.hpp
//  SoundPusher
//
//  Created by Daniel Vollmer on 15/12/2015.
//
//

#ifndef SPDIFAudioEncoder_hpp
#define SPDIFAudioEncoder_hpp

#include <stdexcept>
#include <cstdint>
#include <memory>
#include <vector>

#include "CoreAudio/CoreAudio.h"
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}


struct LibAVException : std::runtime_error { LibAVException(const int error); };


/// Takes an uncompressed, planar input format, and compresses that into the given output format with a codec.
struct SPDIFAudioEncoder
{
  /// The sample type the encoder takes as input (in planar format).
  typedef float SampleT;

  /**
   * @param inFormat Format of the input data. Must be native, planar, packed float.
   * @param channelLayoutTag The channel layout of the input (and final compressed output) channels.
   * @param outFormat The digital, compressed SPDIF output format to be produced by the encoder.
   * @param codecID The libavcodec codec to use for compression.
   */
  SPDIFAudioEncoder(const AudioStreamBasicDescription &inFormat, const AudioChannelLayoutTag channelLayoutTag,
    const AudioStreamBasicDescription &outFormat, const AVCodecID codecID = AV_CODEC_ID_AC3);

  ~SPDIFAudioEncoder();

  /// @return The input format used by the encoder.
  const AudioStreamBasicDescription &GetInFormat() const { return _inFormat; }
  /// @return The digital output format produced by the encoder.
  const AudioStreamBasicDescription &GetOutFormat() const { return _outFormat; }
  /// @return The number of sample frames in a compressed packet.
  uint32_t GetNumFramesPerPacket() const { return _numFramesPerPacket; }

  /// The maximum number of bytes in an SPDIF packet.
  static constexpr uint32_t MaxBytesPerPacket = 6144;

  /// Encodes the given input frames into the outBuffer.
  /**
   * @param numFrames Number of frames of input data. Must be a equal to _numFramesPerPacket.
   * @param inputFrames Pointers to the (planar) input frames. Must contain as many pointers as
   *   _inFormat.mChannelsPerFrame.
   * @param sizeOutBuffer The number of available bytes at outBuffer. Must be at least _outFormat.mBytesPerPacket.
   * @return The number of bytes encoded to outBuffer, or -1 on error.
   */
  uint32_t EncodePacket(const uint32_t numFrames, const SampleT **inputFrames, uint32_t sizeOutBuffer, uint8_t *outBuffer);

protected:
  /// The avio_write function called by the muxer to write an encoded packet.
  static int WritePacketFunc(void *opaque, uint8_t *buf, int buf_size);

  /// Deleter for memory allocated with av_malloc().
  struct AVDeleter { void operator()(void *p) const { av_free(p); } };

  /// The input format to the encoder (i.e. what it requires).
  AudioStreamBasicDescription _inFormat;
  /// For each channel in _inFormat to which libAV channel index it maps.
  std::vector<uint32_t> _input2LibAVChannel;
  /// The output format to the encoder (i.e. what it produces).
  AudioStreamBasicDescription _outFormat;
  /// The muxer context (which also includes the encoder).
  AVFormatContext *_muxer;
  /// The input audio frame (containing multiple frames (samples) in CoreAudio terms).
  AVFrame *_frame;
  /// The buffer for the packet to encode the input frame into.
  std::unique_ptr<uint8_t[], AVDeleter> _packetBuffer;
  /// The encoded packet.
  AVPacket _packet;

  /// Where WritePacketFunc() writes its output.
  uint8_t *_writePacketBuf;
  /// Where WritePacketFunc() takes the output buffer size from.
  uint32_t _writePacketBufSize;

  /// The number of input frames required to produce an output packet.
  uint32_t _numFramesPerPacket;
};

#endif /* SPDIFAudioEncoder_hpp */
