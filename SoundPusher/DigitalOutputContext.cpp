//
//  DigitalOutputContext.cpp
//  VirtualSound
//
//  Created by Daniel Vollmer on 16/12/2015.
//
//

#include <cassert>
#include "TPCircularBuffer.h"

#include "DigitalOutputContext.hpp"


/// @return The best matching input format for an encoder that outputs to outFormat with the given channel layout.
static AudioStreamBasicDescription GetBestInputFormatForOutputFormat(const AudioStreamBasicDescription &outFormat,
  const AudioChannelLayoutTag outChannelLayoutTag)
{
  AudioStreamBasicDescription format = {};
  format.mSampleRate = outFormat.mSampleRate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked; // interleaved
  format.mBytesPerPacket = 1;
  format.mFramesPerPacket = 1;
  format.mChannelsPerFrame = AudioChannelLayoutTag_GetNumberOfChannels(outChannelLayoutTag);
  format.mBytesPerFrame = format.mChannelsPerFrame * sizeof (float);
  format.mBitsPerChannel = 8 * sizeof (float);
  return format;
}


DigitalOutputContext::DigitalOutputContext(AudioObjectID device, AudioObjectID stream,
  const AudioStreamBasicDescription &format, const AudioChannelLayoutTag channelLayoutTag)
: _device(device), _stream(stream), _format(format), _channelLayoutTag(channelLayoutTag)
, _encoder(GetBestInputFormatForOutputFormat(_format, _channelLayoutTag), _channelLayoutTag, _format)
, _deviceIOProcID(nullptr), _isRunning(false)
, _hogger(_device, true), _originalFormat(_stream, format)
{
  { // pre-encode silence (in case we run out of input data)
    std::vector<float> zeros(_encoder.GetNumFramesPerPacket(), 0.0f);
    // encode the same zeros for each channel
    std::vector<const float *> pointers(_encoder.GetInFormat().mChannelsPerFrame, zeros.data());
    
    _encodedSilence.resize(_encoder.MaxBytesPerPacket);
    const auto silenceSize = _encoder.EncodePacket(static_cast<UInt32>(zeros.size()), pointers.data(), _encoder.MaxBytesPerPacket, _encodedSilence.data());
    _encodedSilence.resize(silenceSize);
    if (silenceSize != format.mBytesPerPacket)
      throw std::runtime_error("Encoded silence has wrong size");
  }

  OSStatus status = AudioDeviceCreateIOProcID(_device, DeviceIOProcFunc, this, &_deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException(status);

  TPCircularBufferInit(&_packetBuffer, 3 * _encoder.MaxBytesPerPacket);
}

DigitalOutputContext::~DigitalOutputContext()
{
  if (_isRunning)
    Stop();
  TPCircularBufferCleanup(&_packetBuffer);
  AudioDeviceDestroyIOProcID(_device, _deviceIOProcID);
}


void DigitalOutputContext::EncodeAndAppendPacket(const uint32_t numFrames, const uint32_t numChannels, const float **inputFrames)
{
  assert(numChannels == _encoder.GetInFormat().mChannelsPerFrame);
  int32_t availableBytesInPackedBuffer = 0;
  auto packedBuffer = static_cast<uint8_t *>(TPCircularBufferHead(&_packetBuffer, &availableBytesInPackedBuffer));
  if (packedBuffer && availableBytesInPackedBuffer >= _encoder.MaxBytesPerPacket)
  {
    const auto packedSize = _encoder.EncodePacket(numFrames, inputFrames, availableBytesInPackedBuffer, packedBuffer);
    TPCircularBufferProduce(&_packetBuffer, packedSize);
  }
}


void DigitalOutputContext::Start()
{
  if (_isRunning)
    return;
  OSStatus status = AudioDeviceStart(_device, _deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException(status);
}

void DigitalOutputContext::Stop()
{
  if (!_isRunning)
    return;
  OSStatus status = AudioDeviceStop(_device, _deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException(status);
}


OSStatus DigitalOutputContext::DeviceIOProcFunc(AudioObjectID inDevice, const AudioTimeStamp* inNow,
  const AudioBufferList* inInputData, const AudioTimeStamp* inInputTime, AudioBufferList* outOutputData,
  const AudioTimeStamp* inOutputTime, void* inClientData)
{
  DigitalOutputContext *me = static_cast<DigitalOutputContext *>(inClientData);
  assert(outOutputData->mNumberBuffers == 1);
  assert(outOutputData->mBuffers[0].mDataByteSize <= me->_encodedSilence.size());

  int32_t availableBytes = 0;
  auto buffer = static_cast<uint8_t *>(TPCircularBufferTail(&me->_packetBuffer, &availableBytes));
  if (availableBytes >= me->_format.mBytesPerPacket)
  {
    // consume excess packets to minimize latency
    int32_t numConsumed = me->_format.mBytesPerPacket * (availableBytes / me->_format.mBytesPerPacket);
    if (numConsumed > me->_format.mBytesPerPacket)
      printf("consumed extra packet(s) (%i bytes in total, one is %i)\n", numConsumed, me->_format.mBytesPerPacket);
    buffer += numConsumed - me->_format.mBytesPerPacket; // grab the newest packet
    memcpy(outOutputData->mBuffers[0].mData, buffer, outOutputData->mBuffers[0].mDataByteSize);
    // note: we always consume whole packet(s), even if the request is for less
    TPCircularBufferConsume(&me->_packetBuffer, numConsumed);
  }
  else
  { // provide silence
    printf("silence (%u avail)\n", availableBytes);
    memcpy(outOutputData->mBuffers[0].mData, me->_encodedSilence.data(), outOutputData->mBuffers[0].mDataByteSize);
  }
  return noErr;
}
