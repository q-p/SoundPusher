//
//  ForwardingInputTap.cpp
//  SoundPusher
//
//  Created by Daniel Vollmer on 16/12/2015.
//
//

#include <algorithm>
#include <cassert>
#include <cstring>

#include "ForwardingInputTap.hpp"
#include "DigitalOutputContext.hpp"


ForwardingInputTap::ForwardingInputTap(AudioObjectID device, AudioObjectID stream,
  DigitalOutputContext &outContext)
: _device(device), _stream(stream), _format(outContext.GetInputFormat()), _outContext(outContext)
, _log(DefaultLogger.GetLevel(), "SoundPusher.InIOProc"), _deviceIOProcID(nullptr), _isRunning(false)
{
  OSStatus status = AudioDeviceCreateIOProcID(_device, DeviceIOProcFunc, this, &_deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException("ForwardingInputTap::AudioDeviceCreateIOProcID()", status);
  CAHelper::SetStreamsEnabled(_device, _deviceIOProcID, false, false); // disable any output streams

  static const AudioObjectPropertyAddress BufferFrameSizeAddress = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMaster};
  const UInt32 desiredBufferFrameSize = std::max(UInt32{128}, _outContext.GetNumFramesPerPacket() / 12);
  UInt32 dataSize = sizeof desiredBufferFrameSize;
  status = AudioObjectSetPropertyData(_device, &BufferFrameSizeAddress, 0, NULL, dataSize, &desiredBufferFrameSize);
  if (status != noErr)
    _log.Notice("Could not set buffer frame-size to %u", desiredBufferFrameSize);
  else
    _log.Info("Set buffer frame-size to %u", desiredBufferFrameSize);
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

OSStatus ForwardingInputTap::DeviceIOProcFunc(AudioObjectID inDevice, const AudioTimeStamp* inNow,
  const AudioBufferList* inInputData, const AudioTimeStamp* inInputTime, AudioBufferList* outOutputData,
  const AudioTimeStamp* inOutputTime, void* inClientData)
{
  ForwardingInputTap *me = static_cast<ForwardingInputTap *>(inClientData);
  assert(inInputData->mNumberBuffers == 1);
  assert(inInputData->mBuffers[0].mNumberChannels == me->_format.mChannelsPerFrame);

  const float *input = static_cast<const float *>(inInputData->mBuffers[0].mData);
  const uint32_t available = inInputData->mBuffers[0].mDataByteSize / (me->_format.mChannelsPerFrame * sizeof *input);
  me->_outContext.AppendInputFrames(available, me->_format.mChannelsPerFrame, input);
  return noErr;
}
