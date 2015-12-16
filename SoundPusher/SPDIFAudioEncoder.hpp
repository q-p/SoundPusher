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

  SPDIFAudioEncoder(const AudioStreamBasicDescription &inFormat, const AudioChannelLayoutTag &inChannelLayoutTag, const AudioStreamBasicDescription &outFormat, void *options = nullptr);
  ~SPDIFAudioEncoder();

  const AudioStreamBasicDescription& GetInFormat() const { return _inFormat; }
  const AudioStreamBasicDescription& GetOutFormat() const { return _outFormat; }
  UInt32 GetNumFramesPerPacket() const { return 0; } // FIXME

  /// Encodes the given input frames into the outBuffer.
  /**
   * @param numFrames Number of frames of input data. Must be a equal to _numFramesPerPacket.
   * @param inputFrames Pointers to the (planar) input frames. Must contain as many pointers as
   *   _inFormat.mChannelsPerFrame.
   * @param sizeOutBuffer The number of available bytes at outBuffer. Must be at least _outFormat.mBytesPerPacket.
   * @return The number of bytes encoded to outBuffer, or -1 on error.
   */
  UInt32 EncodePacket(const UInt32 numFrames, const SampleT **inputFrames, UInt32 sizeOutBuffer, void *outBuffer);

protected:
  static int WritePacketFunc(void *opaque, uint8_t *buf, int buf_size);

  /// The input format to the encoder (i.e. what it requires).
  AudioStreamBasicDescription _inFormat;
  /// The output format to the encoder (i.e. what it produces).
  AudioStreamBasicDescription _outFormat;
  /// The muxer context (which also includes the encoder).
  AVFormatContext *_muxer;
  /// The input audio frame (containing multiple frames (samples) in CoreAudio terms).
  AVFrame *_frame;
  /// The buffer for the packet to encode the input frame into.
  uint8_t *_packetBuffer;
  /// The encoded packet.
  AVPacket _packet;

  /// Where WritePacketFunc() dumps its buffer and size
  uint8_t *_writePacketBuf;
  UInt32 _writePacketBufSize;

  static constexpr UInt32 MaxBytesPerPacket = 6144;

  /// The number of input frames required to produce an output packet.
  UInt32 _numFramesPerPacket;
};

#endif /* SPDIFAudioEncoder_hpp */
