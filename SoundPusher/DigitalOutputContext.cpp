//
//  DigitalOutputContext.cpp
//  SoundPusher
//
//  Created by Daniel Vollmer on 16/12/2015.
//
//

#include <cassert>
#include <atomic>
#include <vector>
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
, _inputBufferNumFramesPointer(nullptr),  _extraInputFrameDropRequest(0), _minBufferedFramesAtOutputTime(0)
, _log(DefaultLogger.GetLevel(), "SoundPusher.OutIOProc"), _deviceIOProcID(nullptr), _isRunning(false)
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
  AudioDeviceDestroyIOProcID(_device, _deviceIOProcID);
  TPCircularBufferCleanup(&_packetBuffer);
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
  _cycleCounter = 0;
  _minBufferedFramesAtOutputTime = UINT32_MAX;
  _extraInputFrameDropRequest.store(0, std::memory_order_relaxed);
  OSStatus status = AudioDeviceStart(_device, _deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException("DigitalOutputContext::Start(): AudioDeviceStart()", status);
}

void DigitalOutputContext::Stop()
{
  if (!_isRunning)
    return;
  // reset on stop, because an forwarding tap may re-start before we do (and thus have a chance to reset it).
  _extraInputFrameDropRequest.store(0, std::memory_order_relaxed);
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

  const auto availableInputFrames = me->_inputBufferNumFramesPointer->load(std::memory_order_relaxed);

  int32_t availableBytes = 0;
  auto buffer = static_cast<const uint8_t *>(TPCircularBufferTail(&me->_packetBuffer, &availableBytes));
  const auto packetSize = me->_format.mBytesPerPacket;
  const auto numFramesPerPacket = me->GetNumFramesPerPacket();

  { // update minimum available frames
    const auto availableFrames = availableInputFrames + (availableBytes / packetSize) * numFramesPerPacket;
    if (availableFrames < me->_minBufferedFramesAtOutputTime)
      me->_minBufferedFramesAtOutputTime = availableFrames;
  }

  uint32_t numExtraBytesToConsume = 0;
  if (me->_cycleCounter++ % 64 == 0)
  {
    const auto numInitialMinBufferedFrames = me->_minBufferedFramesAtOutputTime;
    auto numExtraFrames = 0;
    if (numInitialMinBufferedFrames > numFramesPerPacket)
    {
      numExtraFrames = me->_minBufferedFramesAtOutputTime - numFramesPerPacket;
      { // drop buffered output packets
        const auto numExtraPackets = numExtraFrames / numFramesPerPacket;
        numExtraBytesToConsume = numExtraPackets * packetSize;
        buffer += numExtraBytesToConsume;
        availableBytes -= numExtraBytesToConsume;
        numExtraFrames -= numExtraPackets * numFramesPerPacket;
        me->_minBufferedFramesAtOutputTime -= numExtraPackets * numFramesPerPacket;
      }

      if (numExtraFrames > 0)
        me->AddNumRequestedInputFrameDrops(numExtraFrames);
    }

    if (numInitialMinBufferedFrames != me->_minBufferedFramesAtOutputTime)
      me->_log.Info("Observed %u min buffered frames, reduced to %u", numInitialMinBufferedFrames, me->_minBufferedFramesAtOutputTime);

    me->_minBufferedFramesAtOutputTime = UINT32_MAX;
  }

  if (availableBytes >= packetSize)
  {
    memcpy(outOutputData->mBuffers[0].mData, buffer, outOutputData->mBuffers[0].mDataByteSize);
    // note: we always consume whole packet(s), even if the request is for less
    TPCircularBufferConsume(&me->_packetBuffer, packetSize + numExtraBytesToConsume);
  }
  else
  { // not enough data, repeat the previous packet
    const auto bufferBase = static_cast<const uint8_t *>(me->_packetBuffer.buffer);
    buffer = bufferBase + me->_packetBuffer.tail - packetSize;
    if (buffer < bufferBase)
      buffer += me->_packetBuffer.length; // wrap around to 2nd virtual copy at the back
    memcpy(outOutputData->mBuffers[0].mData, buffer, outOutputData->mBuffers[0].mDataByteSize);
    me->_log.Notice("%u/%u available, min buffered frames is %u", availableBytes, outOutputData->mBuffers[0].mDataByteSize, me->_minBufferedFramesAtOutputTime);
  }
  return noErr;
}
