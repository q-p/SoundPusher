//
//  SPDIFAudioEncoder.cpp
//  SoundPusher
//
//  Created by Daniel Vollmer on 15/12/2015.
//
//

#include <string>
#include <cassert>
#include <cstring>
#include <cmath>
#include <type_traits>

#include <os/log.h>

#include "SPDIFAudioEncoder.hpp"

//==================================================================================================
#pragma mark Helpers
//==================================================================================================

static std::string GetAVErrorString(const int error)
{
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(error, buf, sizeof buf);
  return std::string(buf);
}

LibAVException::LibAVException(const int error)
: std::runtime_error(GetAVErrorString(error))
{ }


static uint64_t AudioChannelLayoutTagToAVChannelLayout(AudioChannelLayoutTag channelLayoutTag, std::vector<int> &outInput2LibAVChannel)
{
  outInput2LibAVChannel.resize(AudioChannelLayoutTag_GetNumberOfChannels(channelLayoutTag));
  for (uint32_t i = 0; i < outInput2LibAVChannel.size(); ++i)
    outInput2LibAVChannel[i] = i;
  switch (channelLayoutTag)
  {
    case kAudioChannelLayoutTag_Mono: // C
      return AV_CH_LAYOUT_MONO;
    case kAudioChannelLayoutTag_Stereo: // L R
    {
      const auto AVLayout = AV_CH_LAYOUT_STEREO;
      outInput2LibAVChannel[0] = av_get_channel_layout_channel_index(AVLayout, AV_CH_FRONT_LEFT);
      outInput2LibAVChannel[1] = av_get_channel_layout_channel_index(AVLayout, AV_CH_FRONT_RIGHT);
      return AVLayout;
    }
    case kAudioChannelLayoutTag_MPEG_3_0_A: // L R C
    {
      const auto AVLayout = AV_CH_LAYOUT_SURROUND;
      outInput2LibAVChannel[0] = av_get_channel_layout_channel_index(AVLayout, AV_CH_FRONT_LEFT);
      outInput2LibAVChannel[1] = av_get_channel_layout_channel_index(AVLayout, AV_CH_FRONT_RIGHT);
      outInput2LibAVChannel[2] = av_get_channel_layout_channel_index(AVLayout, AV_CH_FRONT_CENTER);
      return AVLayout;
    }
    case kAudioChannelLayoutTag_AudioUnit_4: // L R Ls Rs
    {
      const auto AVLayout = AV_CH_LAYOUT_QUAD;
      outInput2LibAVChannel[0] = av_get_channel_layout_channel_index(AVLayout, AV_CH_FRONT_LEFT);
      outInput2LibAVChannel[1] = av_get_channel_layout_channel_index(AVLayout, AV_CH_FRONT_RIGHT);
      outInput2LibAVChannel[2] = av_get_channel_layout_channel_index(AVLayout, AV_CH_BACK_LEFT);
      outInput2LibAVChannel[3] = av_get_channel_layout_channel_index(AVLayout, AV_CH_BACK_RIGHT);
      return AVLayout;
    }
    case kAudioChannelLayoutTag_AudioUnit_5_0: // L R Ls Rs C
    {
      const auto AVLayout = AV_CH_LAYOUT_5POINT0;
      outInput2LibAVChannel[0] = av_get_channel_layout_channel_index(AVLayout, AV_CH_FRONT_LEFT);
      outInput2LibAVChannel[1] = av_get_channel_layout_channel_index(AVLayout, AV_CH_FRONT_RIGHT);
      outInput2LibAVChannel[2] = av_get_channel_layout_channel_index(AVLayout, AV_CH_SIDE_LEFT);
      outInput2LibAVChannel[3] = av_get_channel_layout_channel_index(AVLayout, AV_CH_SIDE_RIGHT);
      outInput2LibAVChannel[4] = av_get_channel_layout_channel_index(AVLayout, AV_CH_FRONT_CENTER);
      return AVLayout;
    }
    case kAudioChannelLayoutTag_AudioUnit_5_1: // L R C LFE Ls Rs
    {
      const auto AVLayout = AV_CH_LAYOUT_5POINT1;
      outInput2LibAVChannel[0] = av_get_channel_layout_channel_index(AVLayout, AV_CH_FRONT_LEFT);
      outInput2LibAVChannel[1] = av_get_channel_layout_channel_index(AVLayout, AV_CH_FRONT_RIGHT);
      outInput2LibAVChannel[2] = av_get_channel_layout_channel_index(AVLayout, AV_CH_FRONT_CENTER);
      outInput2LibAVChannel[3] = av_get_channel_layout_channel_index(AVLayout, AV_CH_LOW_FREQUENCY);
      outInput2LibAVChannel[4] = av_get_channel_layout_channel_index(AVLayout, AV_CH_SIDE_LEFT);
      outInput2LibAVChannel[5] = av_get_channel_layout_channel_index(AVLayout, AV_CH_SIDE_RIGHT);
      return AVLayout;
    }
  }
  return 0;
}


static std::vector<double> GetUpmixMatrix(AudioChannelLayoutTag channelLayoutTag, const std::vector<int> &input2LibAVChannel, bool isDLPii)
{
  static constexpr auto InvSqrt2 =   0.7071067811865475;
  static constexpr auto Sqrt3Over2 = 0.8660254037844386;
  const auto numIn = input2LibAVChannel.size();
  const auto numOut = numIn; // for now
  std::vector<double> matrix(numIn * numOut, 0.0);
  for (std::size_t i = 0; i < numIn; ++i)
    matrix[i + input2LibAVChannel[i] * numIn] = 1.0;

  switch (channelLayoutTag)
  {
    case kAudioChannelLayoutTag_Mono: // C
    case kAudioChannelLayoutTag_Stereo: // L R
      break;
    case kAudioChannelLayoutTag_MPEG_3_0_A: // L R C
    {
      auto &left2Center  = matrix[input2LibAVChannel[2] * numIn + input2LibAVChannel[0]];
      auto &right2Center = matrix[input2LibAVChannel[2] * numIn + input2LibAVChannel[1]];
      assert(left2Center == 0.0 && right2Center == 0.0);
      left2Center  = InvSqrt2;
      right2Center = InvSqrt2;
      break;
    }
    case kAudioChannelLayoutTag_AudioUnit_4: // L R Ls Rs
    {
      auto &left2BackLeft   = matrix[input2LibAVChannel[3] * numIn + input2LibAVChannel[0]];
      auto &right2BackRight = matrix[input2LibAVChannel[4] * numIn + input2LibAVChannel[1]];
      assert(left2BackLeft == 0.0 && right2BackRight == 0.0);
      left2BackLeft   = 1.0;
      right2BackRight = 1.0;
      break;
    }
    case kAudioChannelLayoutTag_AudioUnit_5_0: // L R Ls Rs C
    {
      auto &left2Center  = matrix[input2LibAVChannel[4] * numIn + input2LibAVChannel[0]];
      auto &right2Center = matrix[input2LibAVChannel[4] * numIn + input2LibAVChannel[1]];
      assert(left2Center == 0.0 && right2Center == 0.0);
      left2Center  = InvSqrt2;
      right2Center = InvSqrt2;
      auto &left2BackLeft  = matrix[input2LibAVChannel[2] * numIn + input2LibAVChannel[0]];
      auto &right2BackLeft = matrix[input2LibAVChannel[2] * numIn + input2LibAVChannel[1]];
      assert(left2BackLeft == 0.0 && right2BackLeft == 0.0);
      left2BackLeft  = isDLPii ? Sqrt3Over2 : 0.5;
      right2BackLeft = -0.5;
      auto &left2BackRight  = matrix[input2LibAVChannel[3] * numIn + input2LibAVChannel[0]];
      auto &right2BackRight = matrix[input2LibAVChannel[3] * numIn + input2LibAVChannel[1]];
      assert(left2BackRight == 0.0 && right2BackRight == 0.0);
      left2BackRight  = -0.5;
      right2BackRight = isDLPii ? Sqrt3Over2 : 0.5;
      break;
    }
    case kAudioChannelLayoutTag_AudioUnit_5_1: // L R C LFE Ls Rs
    {
      auto &left2Center  = matrix[input2LibAVChannel[2] * numIn + input2LibAVChannel[0]];
      auto &right2Center = matrix[input2LibAVChannel[2] * numIn + input2LibAVChannel[1]];
      assert(left2Center == 0.0 && right2Center == 0.0);
      left2Center  = InvSqrt2;
      right2Center = InvSqrt2;
      auto &left2BackLeft  = matrix[input2LibAVChannel[4] * numIn + input2LibAVChannel[0]];
      auto &right2BackLeft = matrix[input2LibAVChannel[4] * numIn + input2LibAVChannel[1]];
      assert(left2BackLeft == 0.0 && right2BackLeft == 0.0);
      left2BackLeft  = isDLPii ? Sqrt3Over2 : 0.5;
      right2BackLeft = -0.5;
      auto &left2BackRight  = matrix[input2LibAVChannel[5] * numIn + input2LibAVChannel[0]];
      auto &right2BackRight = matrix[input2LibAVChannel[5] * numIn + input2LibAVChannel[1]];
      assert(left2BackRight == 0.0 && right2BackRight == 0.0);
      left2BackRight  = -0.5;
      right2BackRight = isDLPii ? Sqrt3Over2 : 0.5;
      break;
    }
  }
  return matrix;
}

//==================================================================================================
#pragma mark -
#pragma mark SPDIFAudioEncoder
//==================================================================================================

/// don't free the buffer (as we allocate it in a big chunk with other stuff)
static void buffer_no_free(void *opaque, uint8_t *data)
{ }


SPDIFAudioEncoder::SPDIFAudioEncoder(const AudioStreamBasicDescription &inFormat,
  const AudioChannelLayoutTag channelLayoutTag, const AudioStreamBasicDescription &outFormat, os_log_t logger,
  bool useDLPiiUpmix, const AVCodecID codecID)
: _inFormat(inFormat), _outFormat(outFormat),
  _writePacketBuf(nullptr), _writePacketBufSize(0), _writePacketLogger(logger), _numFramesPerPacket(0)
{
  int status = 0;

  assert(inFormat.mChannelsPerFrame == AudioChannelLayoutTag_GetNumberOfChannels(channelLayoutTag));
  static_assert(std::is_same<float, SampleT>::value, "Unexpected sample type");

  _codecContext.reset(avcodec_alloc_context3(avcodec_find_encoder(codecID)));
  if (!_codecContext)
    throw std::runtime_error("Could not allocate AVCodecContext");
  auto *coder = _codecContext.get();
  auto spdifPayloadFactor = static_cast<double>((MaxBytesPerPacket - 4 * _outFormat.mBitsPerChannel / 8)) / MaxBytesPerPacket;
  if (codecID == AV_CODEC_ID_DTS)
  {
    coder->strict_std_compliance = -2;
//    spdifPayloadFactor = 1.0;
  }

  const auto outputBitsPerSec = _outFormat.mSampleRate * _outFormat.mChannelsPerFrame * _outFormat.mBitsPerChannel;
  coder->bit_rate = std::floor(outputBitsPerSec * spdifPayloadFactor);
  coder->sample_fmt = *coder->codec->sample_fmts;
  coder->sample_rate = _inFormat.mSampleRate;
  coder->channel_layout = AudioChannelLayoutTagToAVChannelLayout(channelLayoutTag, _input2LibAVChannel);
  coder->channels = AudioChannelLayoutTag_GetNumberOfChannels(channelLayoutTag);
  coder->time_base = av_make_q(1, coder->sample_rate);
  coder->codec_type = AVMEDIA_TYPE_AUDIO;
  coder->opaque = this; // used by GetEncodeBuffer() to find our _packetBuffer

  AVDictionary *opts = nullptr;
  status = avcodec_open2(coder, coder->codec, &opts);
  av_dict_free(&opts);
  if (status < 0)
    throw LibAVException(status);

  {
    AVFormatContext *muxer = nullptr;
    status = avformat_alloc_output_context2(&muxer, nullptr, "spdif", nullptr);
    _muxer.reset(muxer);
    if (status < 0)
      throw LibAVException(status);
  }
  assert(std::strcmp(_muxer->oformat->name, "spdif") == 0);

  AVStream *stream = avformat_new_stream(_muxer.get(), nullptr);
  if (!stream)
    throw std::runtime_error("Could not allocate AVStream");
  stream->id = 0;
  stream->time_base = coder->time_base;
  status = avcodec_parameters_from_context(stream->codecpar, coder);
  if (status < 0)
    throw LibAVException(status);

  _numFramesPerPacket = coder->frame_size;

  _frame.reset(av_frame_alloc());
  if (!_frame)
    throw std::runtime_error("Could not allocate AVFrame");
  _frame->format = coder->sample_fmt;
  _frame->channel_layout = coder->channel_layout;
  _frame->sample_rate = coder->sample_rate;
  _frame->nb_samples = coder->frame_size;

  const auto coderInputFrameBufferSize = av_samples_get_buffer_size(nullptr, coder->channels, coder->frame_size, coder->sample_fmt, 0);
  _avBuffer.reset(static_cast<uint8_t *>(av_malloc(coderInputFrameBufferSize + 2 * MaxBytesPerPacket + AV_INPUT_BUFFER_PADDING_SIZE)));
  assert(_avBuffer);

  // set up _frame for the converted input samples
  status = avcodec_fill_audio_frame(_frame.get(), coder->channels, coder->sample_fmt, _avBuffer.get(), coderInputFrameBufferSize, 0);
  if (status < 0)
    throw LibAVException(status);
  // set _packetBuffer for the output of the coder
  _packetBuffer.reset(av_buffer_create(_avBuffer.get() + coderInputFrameBufferSize,
    MaxBytesPerPacket + AV_INPUT_BUFFER_PADDING_SIZE, &buffer_no_free, this, 0));
  coder->get_encode_buffer = &GetEncodeBuffer;

  // set up the muxed packet output
  _muxer->pb = avio_alloc_context(_packetBuffer->data + _packetBuffer->size, MaxBytesPerPacket, 1 /* writable */, this,
    nullptr /* read */, WritePacketFunc, nullptr /* seek */);

  _packet.reset(av_packet_alloc());
  _packet->stream_index = 0;

  // set up format conversion
  _swr.reset(swr_alloc_set_opts(nullptr, _frame->channel_layout, coder->sample_fmt, _frame->sample_rate, _frame->channel_layout, AV_SAMPLE_FMT_FLT, _frame->sample_rate, 0, nullptr));
  status = swr_set_channel_mapping(_swr.get(), _input2LibAVChannel.data());
  if (status < 0)
    throw LibAVException(status);
  status = swr_init(_swr.get());
  if (status < 0)
    throw LibAVException(status);
  assert(swr_get_delay(_swr.get(), _frame->sample_rate) == 0);

  _swrUpmix.reset(swr_alloc_set_opts(nullptr, _frame->channel_layout, coder->sample_fmt, _frame->sample_rate, _frame->channel_layout, AV_SAMPLE_FMT_FLT, _frame->sample_rate, 0, nullptr));
  const auto upmixMatrix = GetUpmixMatrix(channelLayoutTag, _input2LibAVChannel, useDLPiiUpmix);
  status = swr_set_matrix(_swrUpmix.get(), upmixMatrix.data(), static_cast<int>(_input2LibAVChannel.size()));
  if (status < 0)
    throw LibAVException(status);
  status = swr_init(_swrUpmix.get());
  if (status < 0)
    throw LibAVException(status);
  assert(swr_get_delay(_swrUpmix.get(), _frame->sample_rate) == 0);

  status = avformat_write_header(_muxer.get(), nullptr);
  if (status < 0)
    throw LibAVException(status);
}

SPDIFAudioEncoder::~SPDIFAudioEncoder()
{
  av_write_trailer(_muxer.get());
}


/// Called through EncodePacket()'s avcodec_encode_audio2() / codec->encode2() / avcodec_send_frame() call
int SPDIFAudioEncoder::GetEncodeBuffer(struct AVCodecContext *s, AVPacket *pkt, int flags)
{
  auto me = static_cast<SPDIFAudioEncoder *>(s->opaque);
  assert(me);
  assert(pkt->size <= MaxBytesPerPacket);
  // here we insert our re-used AVBuffer (and its AVBufferRef)
  pkt->buf = me->_packetBuffer.get();
  pkt->data = pkt->buf->data;
  return 0;
}

/// Called through EncodePacket()'s av_write_frame() call
int SPDIFAudioEncoder::WritePacketFunc(void *opaque, uint8_t *buf, int buf_size)
{
  auto me = static_cast<SPDIFAudioEncoder *>(opaque);
  assert(me);
  // buffer must've been set-up before
  assert(me->_writePacketBuf);
  if (buf_size > me->_writePacketBufSize)
    os_log_info(me->_writePacketLogger, "Writing %i bytes for packet but only %u available", buf_size, me->_writePacketBufSize);
  const auto num = std::min(static_cast<uint32_t>(buf_size), me->_writePacketBufSize);
  std::memcpy(me->_writePacketBuf, buf, num);
  // update to say how much we've written
  me->_writePacketBuf += num;
  me->_writePacketBufSize -= num;
  return buf_size; // we lie about how much we've written here
}

uint32_t SPDIFAudioEncoder::EncodePacket(const uint32_t numFrames, const SampleT *inputFrames, uint32_t sizeOutBuffer, uint8_t *outBuffer, const bool upmix)
{
  if (numFrames != _numFramesPerPacket)
    throw std::invalid_argument("Incorrect number of frames for encoding");

  int status = 0;
  // convert to coder input format
  status = swr_convert(upmix ? _swrUpmix.get() : _swr.get(), _frame->data, _frame->nb_samples, reinterpret_cast<const uint8_t **>(&inputFrames), numFrames);
  if (status < 0)
    throw LibAVException(status);

  // encoded packet (fields will be set by GetEncodeBuffer() callback; setting them now is not supported)
  _packet->data = nullptr;
  _packet->buf = nullptr;
  _packet->size = MaxBytesPerPacket;

  int got_packet = 0;
  status = _codecContext->codec->encode2(_codecContext.get(), _packet.get(), _frame.get(), &got_packet);
  _packet->buf = nullptr; // this avoids the packet freeing the buffer
  if (status < 0)
    throw LibAVException(status);
  if (!got_packet)
    return 0;

  // note: we could also switch the buffer pointers in _muxer->pb to outBuffer directly, but doc says the AVIOContext
  // should be allocated via av_malloc (which we *kinda* do, although we allocate all our buffers in one allocation)
  _writePacketBuf = outBuffer;
  _writePacketBufSize = sizeOutBuffer;
  status = _muxer->oformat->write_packet(_muxer.get(), _packet.get());
  if (status < 0)
    throw LibAVException(status);
  avio_flush(_muxer->pb);
  assert(_writePacketBuf >= outBuffer && _writePacketBuf <= outBuffer + sizeOutBuffer);

  const auto numBytesWritten = static_cast<uint32_t>(_writePacketBuf - outBuffer);
  // paranoia
  _writePacketBuf = nullptr;
  _writePacketBufSize = 0;
  return numBytesWritten;
}
