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
#include "libavutil/opt.h"
#include "libswresample/swresample.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}


struct LibAVException : std::runtime_error { LibAVException(const int error); };


/// Takes an uncompressed, planar input format, and compresses that into the given output format with a codec.
struct SPDIFAudioEncoder
{
  /// The sample type the encoder takes as input (in interleaved format).
  typedef float SampleT;

  /**
   * @param inFormat Format of the input data. Must be native, planar, packed float.
   * @param channelLayoutTag The channel layout of the input (and final compressed output) channels.
   * @param outFormat The digital, compressed SPDIF output format to be produced by the encoder.
   * @param logger The logger to use.
   * @param useDLPiiUpmix Whether to use the Dolby Pro Logic II sqrt(3)/2 for upmixing the respective front channel to
   *   its back channel (this is asymmetric), or whether to use 0.5.
   * @param codecID The libavcodec codec to use for compression.
   */
  SPDIFAudioEncoder(const AudioStreamBasicDescription &inFormat, const AudioChannelLayoutTag channelLayoutTag,
    const AudioStreamBasicDescription &outFormat, os_log_t logger, bool useDLPiiUpmix,
    const AVCodecID codecID = AV_CODEC_ID_AC3);

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
   * @param inputFrames Pointer to the (interleaved) input frames. Must point to numFrames *
   *   _inFormat.mChannelsPerFrame samples.
   * @param sizeOutBuffer The number of available bytes at outBuffer. Must be at least _outFormat.mBytesPerPacket.
   * @param[out] outBuffer The buffer to which to write the encoded packet.
   * @param upmix Whether to upmix the inputFrames while encoding or not.
   * @return The number of bytes encoded to outBuffer, or -1 on error.
   */
  uint32_t EncodePacket(const uint32_t numFrames, const SampleT *inputFrames, uint32_t sizeOutBuffer, uint8_t *outBuffer, const bool upmix);

protected:
  /// The method returning a (the) default buffer for the encoder.
  static int GetEncodeBuffer(struct AVCodecContext *s, AVPacket *pkt, int flags);
  /// The avio_write function called by the muxer to write an encoded packet.
  static int WritePacketFunc(void *opaque, uint8_t *buf, int buf_size);

  /// Deleter for memory allocated with av_malloc().
  struct AVDeleter { void operator()(void *p) const { av_free(p); } };
  /// Deleter for an AVCodecContext.
  struct AVCodecContextDeleter { void operator()(AVCodecContext *p) const { avcodec_free_context(&p); } };
  /// Deleter for an AVFormatContext with a custom IOContext.
  struct AVFormatContextDeleter { void operator()(AVFormatContext *p) const { av_free(p->pb); avformat_free_context(p); } };
  /// Deleter for an AVFrame (allocated with av_frame_alloc()).
  struct AVFrameDeleter { void operator()(AVFrame *p) const { av_frame_free(&p); } };
  /// Deleter for an AVPacket.
  struct AVPacketDeleter { void operator()(AVPacket *p) const { av_packet_free(&p); } };
  /// Deleter for an AVBufferRef.
  struct AVBufferDeleter { void operator()(AVBufferRef *p) const { av_buffer_unref(&p); } };
  /// Deleter for an SwrContext.
  struct AVSwrContextDeleter { void operator()(SwrContext *p) const { swr_free(&p); } };

  /// The input format to the encoder (i.e. what it requires).
  AudioStreamBasicDescription _inFormat;
  /// The output format to the encoder (i.e. what it produces).
  AudioStreamBasicDescription _outFormat;

  /// The codec context.
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> _codecContext;
  /// The muxer context.
  std::unique_ptr<AVFormatContext, AVFormatContextDeleter> _muxer;
  /// The input audio frame (containing multiple frames (samples) in CoreAudio terms), memory owned by _avBuffer.
  std::unique_ptr<AVFrame, AVFrameDeleter> _frame;
  /// The buffer holding 1) the input frames in the format required by the codec, 2) the encoded packet, and 3) the muxed packet.
  std::unique_ptr<uint8_t[], AVDeleter> _avBuffer;
  /// The AVBufferRef for the compressed packet (the middle chunk in _avBuffer).
  std::unique_ptr<AVBufferRef, AVBufferDeleter> _packetBuffer;
  /// The encoded packet.
  std::unique_ptr<AVPacket, AVPacketDeleter> _packet;

  /// The channel-remap from input format to libav channel order, required by the converter.
  std::vector<int> _input2LibAVChannel;
  std::unique_ptr<SwrContext, AVSwrContextDeleter> _swr;
  std::unique_ptr<SwrContext, AVSwrContextDeleter> _swrUpmix;

  /// The number of input frames required to produce an output packet.
  uint32_t _numFramesPerPacket;

  /// Where WritePacketFunc() writes its output.
  uint8_t *_writePacketBuf;
  /// Where WritePacketFunc() takes the output buffer size from.
  uint32_t _writePacketBufSize;
  /// The logger used (if required) when encoding a packet.
  os_log_t _writePacketLogger;
};

#endif /* SPDIFAudioEncoder_hpp */
