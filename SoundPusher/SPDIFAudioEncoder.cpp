//
//  SPDIFAudioEncoder.cpp
//  VirtualSound
//
//  Created by Daniel Vollmer on 15/12/2015.
//
//

#include <string>
#include <cassert>
#include <cstring>
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


static AudioChannelLayoutTag AVChannelLayoutToAudioChannelLayoutTag(uint64_t channelLayout)
{
	switch (channelLayout)
	{
		case AV_CH_LAYOUT_MONO:
			return kAudioChannelLayoutTag_Mono;
		case AV_CH_LAYOUT_STEREO:
			return kAudioChannelLayoutTag_Stereo;
		case AV_CH_LAYOUT_2_1:
			return kAudioChannelLayoutTag_ITU_2_1;
		case AV_CH_LAYOUT_SURROUND:
			return kAudioChannelLayoutTag_AC3_3_0;
		case AV_CH_LAYOUT_2_2:
		case AV_CH_LAYOUT_QUAD:
			return kAudioChannelLayoutTag_ITU_2_2;
		case AV_CH_LAYOUT_4POINT0:
			return kAudioChannelLayoutTag_AC3_3_1;
		case AV_CH_LAYOUT_5POINT0:
		case AV_CH_LAYOUT_5POINT0_BACK:
			return kAudioChannelLayoutTag_ITU_3_2;
		// with LFE
		case AV_CH_LAYOUT_MONO | AV_CH_LOW_FREQUENCY:
			return kAudioChannelLayoutTag_AC3_1_0_1;
		case AV_CH_LAYOUT_STEREO | AV_CH_LOW_FREQUENCY:
			return kAudioChannelLayoutTag_DVD_4;
		case AV_CH_LAYOUT_2_1 | AV_CH_LOW_FREQUENCY:
			return kAudioChannelLayoutTag_AC3_2_1_1;
		case AV_CH_LAYOUT_SURROUND | AV_CH_LOW_FREQUENCY:
			return kAudioChannelLayoutTag_AC3_3_0_1;
		case AV_CH_LAYOUT_2_2 | AV_CH_LOW_FREQUENCY:
		case AV_CH_LAYOUT_QUAD | AV_CH_LOW_FREQUENCY:
			return kAudioChannelLayoutTag_DVD_18;
		case AV_CH_LAYOUT_4POINT0 | AV_CH_LOW_FREQUENCY:
			return kAudioChannelLayoutTag_AC3_3_1_1;
		case AV_CH_LAYOUT_5POINT1:
		case AV_CH_LAYOUT_5POINT1_BACK:
			return kAudioChannelLayoutTag_MPEG_5_1_C;
	}
	return kAudioChannelLayoutTag_Unknown;
}

static uint64_t AudioChannelLayoutTagToAVChannelLayout(AudioChannelLayoutTag channelLayoutTag)
{
	switch (channelLayoutTag)
	{
		case kAudioChannelLayoutTag_Mono:
			return AV_CH_LAYOUT_MONO;
		case kAudioChannelLayoutTag_Stereo:
			return AV_CH_LAYOUT_STEREO;
		case kAudioChannelLayoutTag_ITU_2_1:
			return AV_CH_LAYOUT_2_1;
		case kAudioChannelLayoutTag_AC3_3_0:
			return AV_CH_LAYOUT_SURROUND;
		case kAudioChannelLayoutTag_ITU_2_2:
			return AV_CH_LAYOUT_2_2;
		case kAudioChannelLayoutTag_AC3_3_1:
			return AV_CH_LAYOUT_4POINT0;
		case kAudioChannelLayoutTag_ITU_3_2:
			return AV_CH_LAYOUT_5POINT0;
		// with LFE
		case kAudioChannelLayoutTag_AC3_1_0_1:
			return AV_CH_LAYOUT_MONO | AV_CH_LOW_FREQUENCY;
		case kAudioChannelLayoutTag_DVD_4:
			return AV_CH_LAYOUT_STEREO | AV_CH_LOW_FREQUENCY;
		case kAudioChannelLayoutTag_AC3_2_1_1:
			return AV_CH_LAYOUT_2_1 | AV_CH_LOW_FREQUENCY;
		case kAudioChannelLayoutTag_AC3_3_0_1:
			return AV_CH_LAYOUT_SURROUND | AV_CH_LOW_FREQUENCY;
		case kAudioChannelLayoutTag_DVD_18:
			return AV_CH_LAYOUT_2_2 | AV_CH_LOW_FREQUENCY;
		case kAudioChannelLayoutTag_AC3_3_1_1:
			return AV_CH_LAYOUT_4POINT0 | AV_CH_LOW_FREQUENCY;
		case kAudioChannelLayoutTag_MPEG_5_1_C:
			return AV_CH_LAYOUT_5POINT1;
	}
	return 0;
}

//==================================================================================================
#pragma mark -
#pragma mark SPDIFAudioEncoder
//==================================================================================================

SPDIFAudioEncoder::SPDIFAudioEncoder(const AudioStreamBasicDescription &inFormat, const AudioChannelLayoutTag channelLayoutTag, const AudioStreamBasicDescription &outFormat, void *options)
: _inFormat(inFormat), _outFormat(outFormat), _muxer(nullptr), _frame(nullptr), _packetBuffer(nullptr), _packet(),
  _writePacketBuf(nullptr), _writePacketBufSize(0), _numFramesPerPacket(0)
{
  int status = avformat_alloc_output_context2(&_muxer, nullptr, "spdif", nullptr);
  if (status < 0)
    throw LibAVException(status);
  assert(std::strcmp(_muxer->oformat->name, "spdif") == 0);

  assert(inFormat.mChannelsPerFrame == AudioChannelLayoutTag_GetNumberOfChannels(channelLayoutTag));

  const auto outputBitsPerSec = _outFormat.mSampleRate * _outFormat.mChannelsPerFrame * _outFormat.mBitsPerChannel;

  // set up the packet output
  unsigned char *buffer = static_cast<unsigned char *>(av_malloc(MaxBytesPerPacket));
  _muxer->pb = avio_alloc_context(buffer, MaxBytesPerPacket, 1 /* writable */, this, nullptr /* read */,
    WritePacketFunc, nullptr /* seek */);

  AVStream *stream = avformat_new_stream(_muxer, avcodec_find_encoder(AV_CODEC_ID_AC3));
  assert(stream && stream->codec);

  AVCodecContext *coder = stream->codec;

  coder->bit_rate = outputBitsPerSec; // highest fitting bit-rate, FIXME should subtract SPDIF overhead
  static_assert(std::is_same<float, SampleT>::value, "Unexcepted sample type");
  coder->sample_fmt = AV_SAMPLE_FMT_FLTP; // planar float

	coder->sample_rate = _inFormat.mSampleRate;
	coder->channel_layout = AudioChannelLayoutTagToAVChannelLayout(channelLayoutTag);
	coder->channels = AudioChannelLayoutTag_GetNumberOfChannels(channelLayoutTag);
	coder->time_base = (AVRational){1, coder->sample_rate};

  stream->time_base = coder->time_base;

  status = avcodec_open2(coder, coder->codec, nullptr /* FIXME opts */);
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
  std::memcpy(me->_writePacketBuf, buf, buf_size);
  // set the buffer to null (so we don't accidentally overwrite an old section) and let ourself know how much we wrote
  me->_writePacketBuf += buf_size;
  me->_writePacketBufSize = buf_size;
  return buf_size;
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
    _frame->data[i] = reinterpret_cast<uint8_t *>(const_cast<SampleT *>(inputFrames[i]));
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

  const auto packetSize = _packet.size;
  _writePacketBuf = static_cast<uint8_t *>(outBuffer);
  _writePacketBufSize = sizeOutBuffer;
  status = av_write_frame(_muxer, &_packet);
	if (status < 0)
    throw LibAVException(status);
  assert(_writePacketBuf == static_cast<uint8_t *>(outBuffer) + _writePacketBufSize);
  _writePacketBuf = nullptr; // don't expect another write, except through us

//  printf("muxed packet of size %i into %i bytes\n", packetSize, _writePacketBufSize);

	return _writePacketBufSize;
}