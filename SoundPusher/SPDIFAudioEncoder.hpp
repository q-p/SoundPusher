//
//  SPDIFAudioEncoder.hpp
//  VirtualSound
//
//  Created by Daniel Vollmer on 15/12/2015.
//
//

#ifndef SPDIFAudioEncoder_hpp
#define SPDIFAudioEncoder_hpp

#include "CoreAudio/CoreAudio.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

struct LibAVException : std::runtime_error { LibAVException(const int error); };


struct SPDIFAudioEncoder
{
  typedef float SampleT;

  SPDIFAudioEncoder(const AudioStreamBasicDescription &inFormat, const AudioChannelLayoutTag channelLayoutTag, const AudioStreamBasicDescription &outFormat, void *options = nullptr);
  ~SPDIFAudioEncoder();

  const AudioStreamBasicDescription& GetInFormat() const { return _inFormat; }
  const AudioStreamBasicDescription& GetOutFormat() const { return _outFormat; }
  uint32_t GetNumFramesPerPacket() const { return _numFramesPerPacket; }

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
  static int WritePacketFunc(void *opaque, uint8_t *buf, int buf_size);

  struct AVDeleter { void operator()(void *p) const { av_free(p); } };

  /// The input format to the encoder (i.e. what it requires).
  AudioStreamBasicDescription _inFormat;
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

  /// Where WritePacketFunc() dumps its buffer and size
  uint8_t *_writePacketBuf;
  uint32_t _writePacketBufSize;

  /// The number of input frames required to produce an output packet.
  uint32_t _numFramesPerPacket;
};

#endif /* SPDIFAudioEncoder_hpp */
