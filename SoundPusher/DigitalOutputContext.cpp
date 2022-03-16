//
//  DigitalOutputContext.cpp
//  SoundPusher
//
//  Created by Daniel Vollmer on 16/12/2015.
//
//

#include <cassert>
#include <random>
#include <chrono>
#include <algorithm>

#include "TPCircularBuffer.h"

#include "DigitalOutputContext.hpp"

double DigitalOutputContext::IOCycleSafetyFactor = 8.0;

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

void DigitalOutputContext::SetIOCycleSafetyFactor(const double safetyFactor)
{
  IOCycleSafetyFactor = safetyFactor;
}

double DigitalOutputContext::MeasureSafeIOCycleUsage(const double safetyFactor)
{
  // we take the shortest time (which will have positive cache effects), and multiply this by safetyFactor.
  static constexpr uint32_t NumReps = 16;

  const auto numFramesPerOutputPacket = GetNumFramesPerPacket();
  const auto numFramesPerEncode = _encoder.GetNumFramesPerPacket();
  assert(numFramesPerOutputPacket >= numFramesPerEncode && numFramesPerOutputPacket % numFramesPerEncode == 0);
  const auto numChannels = GetNumInputChannels();
  const auto secsPerPacket = numFramesPerOutputPacket / GetInputFormat().mSampleRate;

  // generate some noise (so we get a worst-case measurement)
  std::vector<float> input(numFramesPerOutputPacket * numChannels);
  {
    std::minstd_rand rng;
    std::uniform_real_distribution<float> distribution(0.0f, 1.0f);
    for (auto &v : input)
      v = distribution(rng);
  }

  std::vector<uint8_t> output(_encoder.MaxBytesPerPacket);
  double minUsage = 1.0 / safetyFactor; // ensures result will at most be 1.0

  // this encodes without upmix (without timing) as well, so we trigger any memory allocations there as well
  _encoder.EncodePacket(numFramesPerEncode, input.data(), static_cast<uint32_t>(output.size()), output.data(), false);

  for (auto i = decltype(NumReps){0}; i < NumReps; ++i)
  {
    const auto start = std::chrono::steady_clock::now();
    const float *in = input.data();
    uint8_t *out = output.data();
    uint32_t spaceAvailable = static_cast<uint32_t>(output.size());
    for (uint32_t n = 0; n < numFramesPerOutputPacket / numFramesPerEncode; ++n)
    {
      const auto bytesWritten = _encoder.EncodePacket(numFramesPerEncode, in, spaceAvailable, out, true /* upmixing is more work */);
      in += numFramesPerEncode * numChannels;
      out += bytesWritten;
      spaceAvailable -= bytesWritten;
    }
    const auto end = std::chrono::steady_clock::now();
    const std::chrono::duration<double> duration = end - start;
    const auto usage = duration.count() / secsPerPacket;
    minUsage = std::min(minUsage, usage);
  }
  return safetyFactor * minUsage;
}

DigitalOutputContext::DigitalOutputContext(AudioObjectID device, AudioObjectID stream,
  const AudioStreamBasicDescription &format, const AudioChannelLayoutTag channelLayoutTag)
: _device(device), _stream(stream), _format(format), _channelLayoutTag(channelLayoutTag)
, _log(os_log_create("de.maven.SoundPusher", "OutIOProc"))
, _encoder(GetBestInputFormatForOutputFormat(_format, _channelLayoutTag), _channelLayoutTag, _format, _log)
, _cycleCounter(0), _minBufferedFramesAtOutputTime(0), _deviceIOProcID(nullptr), _isRunning(false)
, _hogger(_device, true), _originalFormat(_stream, format)
{
  OSStatus status = AudioDeviceCreateIOProcID(_device, DeviceIOProcFunc, this, &_deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException("DigitalOutputContext::AudioDeviceCreateIOProcID()", status);
  CAHelper::SetStreamsEnabled(_device, _deviceIOProcID, true, false); // disable any input streams

  const auto numFramesPerPacket = GetNumFramesPerPacket();
  const auto numInputChannels = GetNumInputChannels();
  TPCircularBufferInit(&_inputBuffer, 4 * numFramesPerPacket * numInputChannels * sizeof (float));
  std::memset(_inputBuffer.buffer, 0, _inputBuffer.length);

  const auto safetyFactor = IOCycleSafetyFactor;
  // this also triggers some memory allocations in the upmixing code
  const Float32 cycleUsage = static_cast<Float32>(MeasureSafeIOCycleUsage(std::max(1.0, safetyFactor)));
  if (safetyFactor >= 1.0)
  {
    static const AudioObjectPropertyAddress IOCycleUsageAddress = {kAudioDevicePropertyIOCycleUsage, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMaster};
    UInt32 dataSize = sizeof cycleUsage;
    status = AudioObjectSetPropertyData(_device, &IOCycleUsageAddress, 0, NULL, dataSize, &cycleUsage);
    if (status != noErr)
      os_log(_log, "Could not set IOCycleUsage to %f", static_cast<double>(cycleUsage));
    else
      os_log_info(_log, "Set estimated IOCycleUsage to %f", static_cast<double>(cycleUsage));
  }
}

DigitalOutputContext::~DigitalOutputContext()
{
  if (_isRunning)
    Stop();
  AudioDeviceDestroyIOProcID(_device, _deviceIOProcID);
  TPCircularBufferCleanup(&_inputBuffer);
  os_release(_log);
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

  const auto numInputFramesRequired = me->GetNumFramesPerPacket(); // or derive from output buffer size
  const auto numInputFramesDesired = numInputFramesRequired + me->_numSafeFrames;
  const auto numInputChannels = me->GetNumInputChannels();
  int32_t availableBytes = 0;
  auto inputBuffer = static_cast<const float *>(TPCircularBufferTail(&me->_inputBuffer, &availableBytes));
  // if the buffer was completely empty, just take data from the start (which might still be silence)
  if (!inputBuffer)
    inputBuffer = static_cast<const float *>(me->_inputBuffer.buffer);
  uint32_t availableInputFrames = availableBytes / (numInputChannels * sizeof *inputBuffer);

  // update minimum available frames
  if (availableInputFrames < me->_minBufferedFramesAtOutputTime)
    me->_minBufferedFramesAtOutputTime = availableInputFrames;

  uint32_t numExtraFramesToConsume = 0;
  if (me->_cycleCounter++ % 64 == 0)
  {
    if (me->_minBufferedFramesAtOutputTime > numInputFramesDesired)
    { // drop any excess input frames to reduce latency (and hope we don't need them later)
      numExtraFramesToConsume = me->_minBufferedFramesAtOutputTime - numInputFramesDesired;
      inputBuffer += numExtraFramesToConsume * numInputChannels;
      availableInputFrames -= numExtraFramesToConsume;
      os_log_info(me->_log, "Observed %u min buffered frames, reduced to %u", me->_minBufferedFramesAtOutputTime, numInputFramesDesired);
    }
    me->_minBufferedFramesAtOutputTime = UINT32_MAX;
  }

  if (availableInputFrames < numInputFramesRequired)
    os_log_info(me->_log, "%u/%u available, min buffered frames is %u", availableInputFrames, numInputFramesRequired, me->_minBufferedFramesAtOutputTime);
  const auto numPackedFrames = me->_encoder.GetNumFramesPerPacket();
  const auto shouldUpmix = me->_shouldUpmix.load(std::memory_order_relaxed);
  auto outputBuffer = static_cast<uint8_t *>(outOutputData->mBuffers[0].mData);
  // this might encode more than we have data for
  for (uint32_t bytesRemaining = outOutputData->mBuffers[0].mDataByteSize; bytesRemaining > 0; )
  {
    const auto bytesWritten = me->_encoder.EncodePacket(numPackedFrames, inputBuffer, bytesRemaining, outputBuffer, shouldUpmix);
    inputBuffer += numPackedFrames * numInputChannels;
    outputBuffer += bytesWritten;
    bytesRemaining -= bytesWritten;
  }
  if (availableInputFrames >= numInputFramesRequired)
    TPCircularBufferConsume(&me->_inputBuffer, (numInputFramesRequired + numExtraFramesToConsume) * numInputChannels * sizeof *inputBuffer);
  // otherwise we don't consume and let the buffer fill up a bit
  return noErr;
}
