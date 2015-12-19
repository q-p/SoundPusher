//
//  ForwardingInputContext.cpp
//  VirtualSound
//
//  Created by Daniel Vollmer on 16/12/2015.
//
//

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>

#include "ForwardingInputContext.hpp"
#include "DigitalOutputContext.hpp"


ForwardingInputContext::ForwardingInputContext(AudioObjectID device, AudioObjectID stream,
  DigitalOutputContext &outContext)
: _device(device), _stream(stream)
, _format(outContext.GetInputFormat()), _outContext(outContext)
, _deviceIOProcID(nullptr), _isRunning(false)
{
  OSStatus status = AudioDeviceCreateIOProcID(_device, DeviceIOProcFunc, this, &_deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException(status);

  _planarFrames.resize(outContext.GetNumFramesPerPacket() * _format.mChannelsPerFrame, 0.0f);
  _planarInputPointers.resize(_format.mChannelsPerFrame);
  for (auto i = decltype(_format.mChannelsPerFrame){0}; i < _format.mChannelsPerFrame; ++i)
    _planarInputPointers[i] = _planarFrames.data() + i * outContext.GetNumFramesPerPacket();
  _numBufferedFrames = 0;
}

ForwardingInputContext::~ForwardingInputContext()
{
  if (_isRunning)
    Stop();
  AudioDeviceDestroyIOProcID(_device, _deviceIOProcID);
}


void ForwardingInputContext::Start()
{
  if (_isRunning)
    return;
  OSStatus status = AudioDeviceStart(_device, _deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException(status);
}

void ForwardingInputContext::Stop()
{
  if (!_isRunning)
    return;
  OSStatus status = AudioDeviceStop(_device, _deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException(status);
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


OSStatus ForwardingInputContext::DeviceIOProcFunc(AudioObjectID inDevice, const AudioTimeStamp* inNow,
  const AudioBufferList* inInputData, const AudioTimeStamp* inInputTime, AudioBufferList* outOutputData,
  const AudioTimeStamp* inOutputTime, void* inClientData)
{
  ForwardingInputContext *me = static_cast<ForwardingInputContext *>(inClientData);
  const auto numFramesPerPacket = me->_outContext.GetNumFramesPerPacket();
  assert(inInputData->mNumberBuffers == 1);
  assert(inInputData->mBuffers[0].mNumberChannels == me->_format.mChannelsPerFrame);
  assert(me->_format.mChannelsPerFrame < Deinterleavers.size());

  const float *input = static_cast<const float *>(inInputData->mBuffers[0].mData);
  uint32_t available = inInputData->mBuffers[0].mDataByteSize / (me->_format.mChannelsPerFrame * sizeof (float));
  while (available > 0)
  {
    assert(me->_numBufferedFrames < numFramesPerPacket);
    { // copy as many samples as we have space for (which might or might not be a full packet)
      const uint32_t bufferSpace = numFramesPerPacket - me->_numBufferedFrames;
      const uint32_t num = std::min(available, bufferSpace);
      Deinterleavers[me->_format.mChannelsPerFrame](num, input, numFramesPerPacket,
        me->_planarFrames.data() + me->_numBufferedFrames);
      me->_numBufferedFrames += num;
      available -= num;
      input += num;
    }
    assert(me->_numBufferedFrames <= numFramesPerPacket);
    if (me->_numBufferedFrames == numFramesPerPacket)
    { // let's encode (greedily, on this (the input) thread
      me->_outContext.EncodeAndAppendPacket(me->_numBufferedFrames,
        static_cast<uint32_t>(me->_planarInputPointers.size()), me->_planarInputPointers.data());
      me->_numBufferedFrames = 0;
    }
  }
  return noErr;
}
