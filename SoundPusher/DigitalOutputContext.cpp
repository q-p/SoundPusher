//
//  DigitalOutputContext.cpp
//  SoundPusher
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

static void Deinterleave1(const uint32_t num, const float *__restrict in, uint32_t outStrideFloat, float *__restrict out)
{
  std::memcpy(out, in, num * sizeof *out);
}

template <uint32_t NumChannels>
static void Deinterleave(const uint32_t num, const float *__restrict in, uint32_t outStrideFloat, float *__restrict out)
{
  for (uint32_t i = 0; i < num; ++i)
  {
    for (uint32_t j = 0; j < NumChannels; ++j)
      out[j * outStrideFloat + i] = in[i * NumChannels + j];
  }
}

DigitalOutputContext::DigitalOutputContext(AudioObjectID device, AudioObjectID stream,
  const AudioStreamBasicDescription &format, const AudioChannelLayoutTag channelLayoutTag)
: _device(device), _stream(stream), _format(format), _channelLayoutTag(channelLayoutTag)
, _encoder(GetBestInputFormatForOutputFormat(_format, _channelLayoutTag), _channelLayoutTag, _format)
, _deinterleaver(nullptr), _cycleCounter(0), _minBufferedFramesAtOutputTime(0)
, _log(DefaultLogger.GetLevel(), "SoundPusher.OutIOProc"), _deviceIOProcID(nullptr), _isRunning(false)
, _hogger(_device, true), _originalFormat(_stream, format)
{
  OSStatus status = AudioDeviceCreateIOProcID(_device, DeviceIOProcFunc, this, &_deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException("DigitalOutputContext::AudioDeviceCreateIOProcID()", status);

  const auto numFramesPerPacket = GetNumFramesPerPacket();
  const auto numInputChannels = GetInputFormat().mChannelsPerFrame;
  _planarFrames.resize(numFramesPerPacket * numInputChannels, 0.0f);
  _planarInputPointers.resize(numInputChannels);
  for (auto i = decltype(numInputChannels){0}; i < numInputChannels; ++i)
    _planarInputPointers[i] = _planarFrames.data() + i * numFramesPerPacket;

  switch (numInputChannels)
  {
    case 1: _deinterleaver = Deinterleave1;   break;
    case 2: _deinterleaver = Deinterleave<2>; break;
    case 3: _deinterleaver = Deinterleave<3>; break;
    case 4: _deinterleaver = Deinterleave<4>; break;
    case 5: _deinterleaver = Deinterleave<5>; break;
    case 6: _deinterleaver = Deinterleave<6>; break;
    default: throw std::runtime_error("No deinterleaver for number of input channels");
  }

  TPCircularBufferInit(&_inputBuffer, 4 * numFramesPerPacket * numInputChannels * sizeof (float));
}

DigitalOutputContext::~DigitalOutputContext()
{
  if (_isRunning)
    Stop();
  AudioDeviceDestroyIOProcID(_device, _deviceIOProcID);
  TPCircularBufferCleanup(&_inputBuffer);
}


void DigitalOutputContext::Start()
{
  if (_isRunning)
    return;
  _cycleCounter = 0;
  _minBufferedFramesAtOutputTime = UINT32_MAX;
  OSStatus status = AudioDeviceStart(_device, _deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException("DigitalOutputContext::Start(): AudioDeviceStart()", status);
}

void DigitalOutputContext::Stop()
{
  if (!_isRunning)
    return;
  // reset on stop, because an forwarding tap may re-start before we do (and thus have a chance to reset it).
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

  const auto numFramesPerPacket = me->GetNumFramesPerPacket();
  const auto numInputChannels = me->GetInputFormat().mChannelsPerFrame;
  int32_t availableBytes = 0;
  auto inputBuffer = static_cast<const float *>(TPCircularBufferTail(&me->_inputBuffer, &availableBytes));
  uint32_t availableInputFrames = availableBytes / (numInputChannels * sizeof *inputBuffer);

  // update minimum available frames
  if (availableInputFrames < me->_minBufferedFramesAtOutputTime)
    me->_minBufferedFramesAtOutputTime = availableInputFrames;

  uint32_t numExtraFramesToConsume = 0;
  if (me->_cycleCounter++ % 64 == 0)
  {
    if (me->_minBufferedFramesAtOutputTime > numFramesPerPacket)
    { // drop any excess input frames to reduce latency (and hope we don't need them later)
      numExtraFramesToConsume = me->_minBufferedFramesAtOutputTime - numFramesPerPacket;
      inputBuffer += numExtraFramesToConsume * numInputChannels;
      availableInputFrames -= numExtraFramesToConsume;
      me->_log.Info("Observed %u min buffered frames, reduced to %u", me->_minBufferedFramesAtOutputTime, numFramesPerPacket);
    }
    me->_minBufferedFramesAtOutputTime = UINT32_MAX;
  }

  if (availableInputFrames >= numFramesPerPacket)
  {
    me->_deinterleaver(numFramesPerPacket, inputBuffer, numFramesPerPacket, me->_planarFrames.data());
    TPCircularBufferConsume(&me->_inputBuffer, (numFramesPerPacket + numExtraFramesToConsume) * numInputChannels * sizeof *inputBuffer);
  }
  else
  { // we re-encode the previous frames if we don't have enough
    me->_log.Notice("%u/%u available, min buffered frames is %u", availableInputFrames, numFramesPerPacket, me->_minBufferedFramesAtOutputTime);
  }
  auto outputBuffer = static_cast<uint8_t *>(outOutputData->mBuffers[0].mData);
  me->_encoder.EncodePacket(numFramesPerPacket, me->_planarInputPointers.data(), outOutputData->mBuffers[0].mDataByteSize, outputBuffer);

  return noErr;
}
