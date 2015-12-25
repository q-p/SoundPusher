//
//  ForwardingInputTap.cpp
//  SoundPusher
//
//  Created by Daniel Vollmer on 16/12/2015.
//
//

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>

#include "ForwardingInputTap.hpp"
#include "DigitalOutputContext.hpp"


/// @return the number of input frames returned by the device in a nominal cycle.
static uint32_t GetDeviceInputFrameSize(AudioObjectID device)
{
  const AudioObjectPropertyAddress DeviceFrameSizeAddress = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMaster};
  UInt32 deviceFrameSize = 0;
  UInt32 dataSize = sizeof deviceFrameSize;
  OSStatus status = AudioObjectGetPropertyData(device, &DeviceFrameSizeAddress, 0, NULL, &dataSize, &deviceFrameSize);
  if (status != noErr)
    throw CAHelper::CoreAudioException("GetDeviceInputFrameSize(): AudioObjectGetPropertyData()", status);
  return deviceFrameSize;
}

/// @return the minimum number of input frames required to be available at output time.
static uint32_t GetMinFrames(const uint32_t packetSize, const uint32_t framesPerInputCycle)
{
  const uint32_t minInputCyclesPerOutputCycle = packetSize / framesPerInputCycle; // truncating
  return packetSize - minInputCyclesPerOutputCycle * framesPerInputCycle;
}

ForwardingInputTap::ForwardingInputTap(AudioObjectID device, AudioObjectID stream, DigitalOutputContext &outContext)
: _device(device), _stream(stream), _format(outContext.GetInputFormat()), _outContext(outContext)
, _deviceIOProcID(nullptr), _isRunning(false)
{
  OSStatus status = AudioDeviceCreateIOProcID(_device, DeviceIOProcFunc, this, &_deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException("ForwardingInputTap::AudioDeviceCreateIOProcID()", status);

  _numBufferedFrames.store(0, std::memory_order_relaxed);
  _outContext.SetInputBufferNumFramesPointer(&_numBufferedFrames, 
    GetMinFrames(outContext.GetNumFramesPerPacket(), GetDeviceInputFrameSize(_device)));

  _planarFrames.resize(outContext.GetNumFramesPerPacket() * _format.mChannelsPerFrame, 0.0f);
  _planarInputPointers.resize(_format.mChannelsPerFrame);
  for (auto i = decltype(_format.mChannelsPerFrame){0}; i < _format.mChannelsPerFrame; ++i)
    _planarInputPointers[i] = _planarFrames.data() + i * outContext.GetNumFramesPerPacket();
}

ForwardingInputTap::~ForwardingInputTap()
{
  if (_isRunning)
    Stop();
  AudioDeviceDestroyIOProcID(_device, _deviceIOProcID);
}


void ForwardingInputTap::Start()
{
  if (_isRunning)
    return;
  OSStatus status = AudioDeviceStart(_device, _deviceIOProcID);;
  if (status != noErr)
    throw CAHelper::CoreAudioException("ForwardingInputTap::Start(): AudioDeviceStart()", status);
}

void ForwardingInputTap::Stop()
{
  if (!_isRunning)
    return;
  OSStatus status = AudioDeviceStop(_device, _deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException("ForwardingInputTap::Stop(): AudioDeviceStop()", status);
}


static void Deinterleave1(const uint32_t num, const float *in, uint32_t outStrideFloat, float *out)
{
  std::memcpy(out, in, num * sizeof *out);
}

template <uint32_t NumChannels>
static void Deinterleave(const uint32_t num, const float *in, uint32_t outStrideFloat, float *out)
{
  for (uint32_t i = 0; i < num; ++i)
  {
    for (uint32_t j = 0; j < NumChannels; ++j)
      out[j * outStrideFloat + i] = in[i * NumChannels + j];
  }
}

typedef void (*DeinterleaveFunc)(const uint32_t num, const float *in, uint32_t outStrideFloat, float *out);

static const std::array<DeinterleaveFunc, 7> Deinterleavers = {{
  nullptr,
  Deinterleave1,
  Deinterleave<2>,
  Deinterleave<3>,
  Deinterleave<4>,
  Deinterleave<5>,
  Deinterleave<6>
}};


OSStatus ForwardingInputTap::DeviceIOProcFunc(AudioObjectID inDevice, const AudioTimeStamp* inNow,
  const AudioBufferList* inInputData, const AudioTimeStamp* inInputTime, AudioBufferList* outOutputData,
  const AudioTimeStamp* inOutputTime, void* inClientData)
{
  ForwardingInputTap *me = static_cast<ForwardingInputTap *>(inClientData);
  const auto numFramesPerPacket = me->_outContext.GetNumFramesPerPacket();
  assert(inInputData->mNumberBuffers == 1);
  assert(inInputData->mBuffers[0].mNumberChannels == me->_format.mChannelsPerFrame);
  assert(me->_format.mChannelsPerFrame < Deinterleavers.size());

  const float *input = static_cast<const float *>(inInputData->mBuffers[0].mData);
  uint32_t available = inInputData->mBuffers[0].mDataByteSize / (me->_format.mChannelsPerFrame * sizeof (float));
  auto numBufferedFrames = me->_numBufferedFrames.load(std::memory_order_relaxed);
  while (available > 0)
  {
    assert(numBufferedFrames < numFramesPerPacket);
    { // copy as many samples as we have space for (which might or might not be a full packet)
      const uint32_t bufferSpace = numFramesPerPacket - numBufferedFrames;
      const uint32_t num = std::min(available, bufferSpace);
      Deinterleavers[me->_format.mChannelsPerFrame](num, input, numFramesPerPacket,
        me->_planarFrames.data() + numBufferedFrames);
      numBufferedFrames += num;
      available -= num;
      input += num * me->_format.mChannelsPerFrame;
    }
    assert(numBufferedFrames <= numFramesPerPacket);
    if (numBufferedFrames == numFramesPerPacket)
    { // let's encode (greedily, on this (the input) thread
      me->_outContext.EncodeAndAppendPacket(numBufferedFrames,
        static_cast<uint32_t>(me->_planarInputPointers.size()), me->_planarInputPointers.data());
      numBufferedFrames = 0;
    }
  }
  me->_numBufferedFrames.store(numBufferedFrames, std::memory_order_relaxed);
  return noErr;
}
