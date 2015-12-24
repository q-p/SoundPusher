//
//  DigitalOutputContext.cpp
//  VirtualSound
//
//  Created by Daniel Vollmer on 16/12/2015.
//
//

#include <cassert>
#include <atomic>
#include <vector>
#include "TPCircularBuffer.h"

#include "DigitalOutputContext.hpp"
#include "MiniLogger.hpp"


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
, _inputBufferNumFramesPointer(nullptr), _minInputFramesAtOutputTime(0)
, _deviceIOProcID(nullptr), _isRunning(false)
, _hogger(_device, true), _originalFormat(_stream, format)
{
  OSStatus status = AudioDeviceCreateIOProcID(_device, DeviceIOProcFunc, this, &_deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException("DigitalOutputContext::AudioDeviceCreateIOProcID()", status);

  TPCircularBufferInit(&_packetBuffer, 3 * _encoder.MaxBytesPerPacket);

  { // pre-encode silence (in case we run out of input data)
    std::vector<float> zeros(_encoder.GetNumFramesPerPacket(), 0.0f);
    // encode the same zeros for each channel
    std::vector<const float *> pointers(_encoder.GetInFormat().mChannelsPerFrame, zeros.data());

    int32_t availableBytes = 0;
    auto buffer = static_cast<uint8_t *>(TPCircularBufferHead(&_packetBuffer, &availableBytes));
    const auto silenceSize = _encoder.EncodePacket(static_cast<UInt32>(zeros.size()), pointers.data(), availableBytes, buffer);
    if (silenceSize != format.mBytesPerPacket)
    {
      TPCircularBufferCleanup(&_packetBuffer);
      throw std::runtime_error("Encoded silence has wrong size");
    }
    // notify the buffer of the inserted silence packet
    TPCircularBufferProduce(&_packetBuffer, silenceSize);
    // and immediately consume it; now the silence is the "previous" packet
    TPCircularBufferConsume(&_packetBuffer, silenceSize);
  }
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
    throw CAHelper::CoreAudioException("DigitalOutputContext::Start(): AudioDeviceStart()", status);
}

void DigitalOutputContext::Stop()
{
  if (!_isRunning)
    return;
  OSStatus status = AudioDeviceStop(_device, _deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException("DigitalOutputContext::Stop(): AudioDeviceStop()", status);
}


OSStatus DigitalOutputContext::DeviceIOProcFunc(AudioObjectID inDevice, const AudioTimeStamp* inNow,
  const AudioBufferList* inInputData, const AudioTimeStamp* inInputTime, AudioBufferList* outOutputData,
  const AudioTimeStamp* inOutputTime, void* inClientData)
{
  DigitalOutputContext *me = static_cast<DigitalOutputContext *>(inClientData);
  assert(outOutputData->mNumberBuffers == 1);

  int32_t availableBytes = 0;
  auto buffer = static_cast<const uint8_t *>(TPCircularBufferTail(&me->_packetBuffer, &availableBytes));
  const auto packetSize = me->_format.mBytesPerPacket;
  if (availableBytes >= packetSize)
  {
    uint32_t numExtraBytesToConsume = 0;
    const uint32_t numExtraPackets = (availableBytes / packetSize) - 1u;

    // optimize output latency by consuming excess compressed packets
    if (numExtraPackets > 0)
      numExtraBytesToConsume = numExtraPackets * packetSize;

    memcpy(outOutputData->mBuffers[0].mData, buffer + numExtraBytesToConsume, outOutputData->mBuffers[0].mDataByteSize);
    // note: we always consume whole packet(s), even if the request is for less
    TPCircularBufferConsume(&me->_packetBuffer, packetSize + numExtraBytesToConsume);

    // attempt reduce extra input latency (i.e. drop some input buffer contents right now)
    const auto availableInputFrames = me->_inputBufferNumFramesPointer->load(std::memory_order_relaxed);
    if (availableInputFrames > me->_minInputFramesAtOutputTime)
    {
      DefaultLogger.Info("OutIOProc: %u input frames at output time, reducing to %u frames\n", availableInputFrames, me->_minInputFramesAtOutputTime);
      me->_inputBufferNumFramesPointer->store(me->_minInputFramesAtOutputTime, std::memory_order_relaxed);
      // we drop the newer frames instead of the stale ones so we don't have to copy around
    }
  }
  else
  { // not enough data, repeat the previous packet
    const auto bufferBase = static_cast<const uint8_t *>(me->_packetBuffer.buffer);
    buffer = bufferBase + me->_packetBuffer.tail - packetSize;
    if (buffer < bufferBase)
      buffer += me->_packetBuffer.length; // wrap around to 2nd virtual copy at the back
    DefaultLogger.Info("OutIOProc: %u/%u available, min input frames is %u\n", availableBytes, outOutputData->mBuffers[0].mDataByteSize, me->_minInputFramesAtOutputTime);
    memcpy(outOutputData->mBuffers[0].mData, buffer, outOutputData->mBuffers[0].mDataByteSize);
  }
  return noErr;
}
