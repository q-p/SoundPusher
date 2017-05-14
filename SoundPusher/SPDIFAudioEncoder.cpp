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

#include "SPDIFAudioEncoder.hpp"
#include "MiniLogger.hpp"

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


static std::vector<double> GetUpmixMatrix(AudioChannelLayoutTag channelLayoutTag, const std::vector<int> &input2LibAVChannel)
{
  static constexpr auto InvSqr2 = 0.7071067811865475;
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
      left2Center  = InvSqr2;
      right2Center = InvSqr2;
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
      left2Center  = InvSqr2;
      right2Center = InvSqr2;
      auto &left2BackLeft  = matrix[input2LibAVChannel[2] * numIn + input2LibAVChannel[0]];
      auto &right2BackLeft = matrix[input2LibAVChannel[2] * numIn + input2LibAVChannel[1]];
      assert(left2BackLeft == 0.0 && right2BackLeft == 0.0);
      left2BackLeft  =  0.5;
      right2BackLeft = -0.5;
      auto &left2BackRight  = matrix[input2LibAVChannel[3] * numIn + input2LibAVChannel[0]];
      auto &right2BackRight = matrix[input2LibAVChannel[3] * numIn + input2LibAVChannel[1]];
      assert(left2BackRight == 0.0 && right2BackRight == 0.0);
      left2BackRight  = -0.5;
      right2BackRight =  0.5;
      break;
    }
    case kAudioChannelLayoutTag_AudioUnit_5_1: // L R C LFE Ls Rs
    {
      auto &left2Center  = matrix[input2LibAVChannel[2] * numIn + input2LibAVChannel[0]];
      auto &right2Center = matrix[input2LibAVChannel[2] * numIn + input2LibAVChannel[1]];
      assert(left2Center == 0.0 && right2Center == 0.0);
      left2Center  = InvSqr2;
      right2Center = InvSqr2;
      auto &left2BackLeft  = matrix[input2LibAVChannel[4] * numIn + input2LibAVChannel[0]];
      auto &right2BackLeft = matrix[input2LibAVChannel[4] * numIn + input2LibAVChannel[1]];
      assert(left2BackLeft == 0.0 && right2BackLeft == 0.0);
      left2BackLeft  =  0.5;
      right2BackLeft = -0.5;
      auto &left2BackRight  = matrix[input2LibAVChannel[5] * numIn + input2LibAVChannel[0]];
      auto &right2BackRight = matrix[input2LibAVChannel[5] * numIn + input2LibAVChannel[1]];
      assert(left2BackRight == 0.0 && right2BackRight == 0.0);
      left2BackRight  = -0.5;
      right2BackRight =  0.5;
      break;
    }
  }
  return matrix;
}

//==================================================================================================
#pragma mark -
#pragma mark SPDIFAudioEncoder
//==================================================================================================

SPDIFAudioEncoder::SPDIFAudioEncoder(const AudioStreamBasicDescription &inFormat,
  const AudioChannelLayoutTag channelLayoutTag, const AudioStreamBasicDescription &outFormat, MiniLogger &logger,
  const AVCodecID codecID)
: _inFormat(inFormat), _outFormat(outFormat), _packetBuffer(nullptr), _packet(),
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
  coder->time_base = (AVRational){1, coder->sample_rate};
  coder->codec_type = AVMEDIA_TYPE_AUDIO;

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
  _avBuffer.reset(static_cast<uint8_t *>(av_malloc(coderInputFrameBufferSize + 2 * MaxBytesPerPacket)));
  assert(_avBuffer);

  // set up _frame for the converted input samples
  status = avcodec_fill_audio_frame(_frame.get(), coder->channels, coder->sample_fmt, _avBuffer.get(), coderInputFrameBufferSize, 0);
  if (status < 0)
    throw LibAVException(status);
  // set _packetBuffer for the output of the coder
  _packetBuffer = _avBuffer.get() + coderInputFrameBufferSize;
  // set up the muxed packet output
  _muxer->pb = avio_alloc_context(_packetBuffer + MaxBytesPerPacket, MaxBytesPerPacket, 1 /* writable */, this, nullptr /* read */,
    WritePacketFunc, nullptr /* seek */);

  av_init_packet(&_packet);
  _packet.stream_index = 0;

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
  const auto upmixMatrix = GetUpmixMatrix(channelLayoutTag, _input2LibAVChannel);
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


/// Called through EncodePacket()'s av_write_frame() call
int SPDIFAudioEncoder::WritePacketFunc(void *opaque, uint8_t *buf, int buf_size)
{
  auto me = static_cast<SPDIFAudioEncoder *>(opaque);
  assert(me);
  // buffer must've been set-up before
  assert(me->_writePacketBuf);
  if (buf_size > me->_writePacketBufSize)
    me->_writePacketLogger.Info("Writing %i bytes for packet but only %u available", buf_size, me->_writePacketBufSize);
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

  // encoded packet
  _packet.data = _packetBuffer;
  _packet.size = MaxBytesPerPacket;

  status = avcodec_send_frame(_codecContext.get(), _frame.get());
  if (status < 0)
    throw LibAVException(status);
  status = avcodec_receive_packet(_codecContext.get(), &_packet);
  if (status == AVERROR(EAGAIN))
    return 0; // no packet data
  if (status < 0)
    throw LibAVException(status);

  if (_packet.size <= 0)
    return 0;

  _writePacketBuf = outBuffer;
  _writePacketBufSize = sizeOutBuffer;
  status = av_write_frame(_muxer.get(), &_packet);
  if (status < 0)
    throw LibAVException(status);
  assert(_writePacketBuf >= outBuffer && _writePacketBuf <= outBuffer + sizeOutBuffer);

  const auto numBytesWritten = static_cast<uint32_t>(_writePacketBuf - outBuffer);
  // paranoia
  _writePacketBuf = nullptr;
  _writePacketBufSize = 0;
  return numBytesWritten;
}
