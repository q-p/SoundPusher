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


static uint64_t AudioChannelLayoutTagToAVChannelLayout(AudioChannelLayoutTag channelLayoutTag, std::vector<uint32_t> &outInput2LibAVChannel)
{
  outInput2LibAVChannel.resize(AudioChannelLayoutTag_GetNumberOfChannels(channelLayoutTag));
  for (uint32_t i = 0; i < outInput2LibAVChannel.size(); ++i)
    outInput2LibAVChannel[i] = i;
  switch (channelLayoutTag)
  {
    case kAudioChannelLayoutTag_Mono: // C
      return AV_CH_LAYOUT_MONO;
    case kAudioChannelLayoutTag_Stereo: // L R
      outInput2LibAVChannel[0] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_STEREO, AV_CH_FRONT_LEFT);
      outInput2LibAVChannel[1] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_STEREO, AV_CH_FRONT_RIGHT);
      return AV_CH_LAYOUT_STEREO;
    case kAudioChannelLayoutTag_MPEG_3_0_A: // L R C
      outInput2LibAVChannel[0] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_SURROUND, AV_CH_FRONT_LEFT);
      outInput2LibAVChannel[1] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_SURROUND, AV_CH_FRONT_RIGHT);
      outInput2LibAVChannel[2] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_SURROUND, AV_CH_FRONT_CENTER);
      return AV_CH_LAYOUT_SURROUND;
    case kAudioChannelLayoutTag_AudioUnit_4: // L R Ls Rs
      outInput2LibAVChannel[0] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_QUAD, AV_CH_FRONT_LEFT);
      outInput2LibAVChannel[1] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_QUAD, AV_CH_FRONT_RIGHT);
      outInput2LibAVChannel[2] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_QUAD, AV_CH_BACK_LEFT);
      outInput2LibAVChannel[3] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_QUAD, AV_CH_BACK_RIGHT);
      return AV_CH_LAYOUT_QUAD;
    case kAudioChannelLayoutTag_AudioUnit_5_0: // L R Ls Rs C
      outInput2LibAVChannel[0] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_5POINT0_BACK, AV_CH_FRONT_LEFT);
      outInput2LibAVChannel[1] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_5POINT0_BACK, AV_CH_FRONT_RIGHT);
      outInput2LibAVChannel[2] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_5POINT0_BACK, AV_CH_BACK_LEFT);
      outInput2LibAVChannel[3] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_5POINT0_BACK, AV_CH_BACK_RIGHT);
      outInput2LibAVChannel[4] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_5POINT0_BACK, AV_CH_FRONT_CENTER);
      return AV_CH_LAYOUT_5POINT0_BACK;
    case kAudioChannelLayoutTag_AudioUnit_5_1: // L R C LFE Ls Rs
      outInput2LibAVChannel[0] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_5POINT1_BACK, AV_CH_FRONT_LEFT);
      outInput2LibAVChannel[1] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_5POINT1_BACK, AV_CH_FRONT_RIGHT);
      outInput2LibAVChannel[2] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_5POINT1_BACK, AV_CH_FRONT_CENTER);
      outInput2LibAVChannel[3] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_5POINT1_BACK, AV_CH_LOW_FREQUENCY);
      outInput2LibAVChannel[4] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_5POINT1_BACK, AV_CH_BACK_LEFT);
      outInput2LibAVChannel[5] = av_get_channel_layout_channel_index(AV_CH_LAYOUT_5POINT1_BACK, AV_CH_BACK_RIGHT);
      return AV_CH_LAYOUT_5POINT1_BACK;
  }
  return 0;
}

//==================================================================================================
#pragma mark -
#pragma mark SPDIFAudioEncoder
//==================================================================================================

SPDIFAudioEncoder::SPDIFAudioEncoder(const AudioStreamBasicDescription &inFormat,
  const AudioChannelLayoutTag channelLayoutTag, const AudioStreamBasicDescription &outFormat, const AVCodecID codecID)
: _inFormat(inFormat), _outFormat(outFormat), _muxer(nullptr), _frame(nullptr), _packetBuffer(nullptr), _packet(),
  _writePacketBuf(nullptr), _writePacketBufSize(0), _numFramesPerPacket(0)
{
  int status = avformat_alloc_output_context2(&_muxer, nullptr, "spdif", nullptr);
  if (status < 0)
    throw LibAVException(status);
  assert(std::strcmp(_muxer->oformat->name, "spdif") == 0);

  assert(inFormat.mChannelsPerFrame == AudioChannelLayoutTag_GetNumberOfChannels(channelLayoutTag));

  // set up the packet output
  unsigned char *buffer = static_cast<unsigned char *>(av_malloc(MaxBytesPerPacket));
  _muxer->pb = avio_alloc_context(buffer, MaxBytesPerPacket, 1 /* writable */, this, nullptr /* read */,
    WritePacketFunc, nullptr /* seek */);

  AVStream *stream = avformat_new_stream(_muxer, avcodec_find_encoder(codecID));
  assert(stream && stream->codec);

  AVCodecContext *coder = stream->codec;
  const auto spdifPayloadFactor = static_cast<double>((MaxBytesPerPacket - 4 * _outFormat.mBitsPerChannel / 8)) / MaxBytesPerPacket;
  const auto outputBitsPerSec = _outFormat.mSampleRate * _outFormat.mChannelsPerFrame * _outFormat.mBitsPerChannel;
  coder->bit_rate = std::floor(outputBitsPerSec * spdifPayloadFactor);
  static_assert(std::is_same<float, SampleT>::value, "Unexcepted sample type");
  coder->sample_fmt = AV_SAMPLE_FMT_FLTP; // planar float

  coder->sample_rate = _inFormat.mSampleRate;
  coder->channel_layout = AudioChannelLayoutTagToAVChannelLayout(channelLayoutTag, _input2LibAVChannel);
  coder->channels = AudioChannelLayoutTag_GetNumberOfChannels(channelLayoutTag);
  coder->time_base = (AVRational){1, coder->sample_rate};

  stream->time_base = coder->time_base;

  AVDictionary *opts = nullptr;
  status = avcodec_open2(coder, coder->codec, &opts);
  av_dict_free(&opts);
  if (status < 0)
  {
    av_free(_muxer->pb->buffer); // allocated by us, not avio
    av_free(_muxer->pb); // same
    avformat_free_context(_muxer);
    throw LibAVException(status);
  }
  _numFramesPerPacket = coder->frame_size;

  _frame = av_frame_alloc();
  assert(_frame);
  _frame->format = coder->sample_fmt;
  _frame->channel_layout = coder->channel_layout;
  _frame->sample_rate = coder->sample_rate;
  _frame->nb_samples = coder->frame_size;
  _frame->linesize[0] = _frame->nb_samples * sizeof(SampleT);

  _packetBuffer.reset(static_cast<uint8_t *>(av_malloc(MaxBytesPerPacket)));

  av_init_packet(&_packet);
  _packet.stream_index = 0;

  status = avformat_write_header(_muxer, nullptr);
  if (status < 0)
  {
    av_frame_free(&_frame);
    av_free(_muxer->pb->buffer); // allocated by us, not avio
    av_free(_muxer->pb); // same
    avformat_free_context(_muxer);
    throw LibAVException(status);
  }
}

SPDIFAudioEncoder::~SPDIFAudioEncoder()
{
  av_write_trailer(_muxer);
  av_frame_free(&_frame);
  if (_muxer)
  {
    avcodec_close(_muxer->streams[0]->codec);
    av_free(_muxer->pb->buffer); // allocated by us, not avio
    _muxer->pb->buffer = nullptr;
    av_free(_muxer->pb); // same
    _muxer->pb = nullptr;
    avformat_free_context(_muxer);
    _muxer = nullptr;
  }
}


/// Called through EncodePacket()'s av_write_frame() call
int SPDIFAudioEncoder::WritePacketFunc(void *opaque, uint8_t *buf, int buf_size)
{
  auto me = static_cast<SPDIFAudioEncoder *>(opaque);
  assert(me);
  // buffer must've been set-up before
  assert(me->_writePacketBuf && me->_writePacketBufSize >= buf_size);
  const auto num = std::min(static_cast<uint32_t>(buf_size), me->_writePacketBufSize);
  std::memcpy(me->_writePacketBuf, buf, num);
  // update to say how much we've written
  me->_writePacketBuf += num;
  me->_writePacketBufSize -= num;
  return buf_size; // we lie about how much we've written here
}

uint32_t SPDIFAudioEncoder::EncodePacket(const uint32_t numFrames, const SampleT **inputFrames, uint32_t sizeOutBuffer, uint8_t *outBuffer)
{
  if (numFrames != _numFramesPerPacket)
    throw std::invalid_argument("Incorrect number of frames for encoding");
  AVCodecContext *coder = _muxer->streams[0]->codec;

  // input frame
  _frame->nb_samples = numFrames;
  for (int i = 0; i < coder->channels; ++i)
  {
    _frame->data[i] = reinterpret_cast<uint8_t *>(const_cast<SampleT *>(inputFrames[_input2LibAVChannel[i]]));
    _frame->linesize[i] = numFrames * sizeof(SampleT);
  }

  // output packet
  _packet.data = _packetBuffer.get();
  _packet.size = MaxBytesPerPacket;

  int got_packet = 0;
  int status = avcodec_encode_audio2(coder, &_packet, _frame, &got_packet);

  if (status < 0)
    throw LibAVException(status);

  if (!got_packet)
    return 0;

  _writePacketBuf = outBuffer;
  _writePacketBufSize = sizeOutBuffer;
  status = av_write_frame(_muxer, &_packet);
  if (status < 0)
    throw LibAVException(status);
  assert(_writePacketBuf >= outBuffer && _writePacketBuf <= outBuffer + sizeOutBuffer);

  const auto numBytesWritten = static_cast<uint32_t>(_writePacketBuf - outBuffer);
  // paranoia
  _writePacketBuf = nullptr;
  _writePacketBufSize = 0;
  return numBytesWritten;
}