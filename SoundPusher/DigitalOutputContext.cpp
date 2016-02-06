//
//  DigitalOutputContext.cpp
//  SoundPusher
//
//  Created by Daniel Vollmer on 16/12/2015.
//
//

#include <cassert>
#include <chrono>

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

template <uint32_t NumChannels>
static void Deinterleave(const uint32_t num, const float *__restrict in, uint32_t outStrideFloat, float *__restrict out)
{
  for (uint32_t i = 0; i < num; ++i)
  {
    for (uint32_t j = 0; j < NumChannels; ++j)
      out[j * outStrideFloat + i] = in[i * NumChannels + j];
  }
}

template <>
void Deinterleave<1>(const uint32_t num, const float *__restrict in, uint32_t outStrideFloat, float *__restrict out)
{
  std::memcpy(out, in, num * sizeof *out);
}

static void DeinterleaveUpmix6(const uint32_t num, const float *__restrict in, uint32_t outStrideFloat, float *__restrict out)
{
  static constexpr float InvSqr2 = 0.7071067811865475f;
  // channel order: L R C LFE Ls Rs (kAudioChannelLayoutTag_AudioUnit_5_1)
  static constexpr uint32_t N   = 6;
  static constexpr uint32_t L   = 0;
  static constexpr uint32_t R   = 1;
  static constexpr uint32_t C   = 2;
  static constexpr uint32_t LFE = 3;
  static constexpr uint32_t Ls  = 4;
  static constexpr uint32_t Rs  = 5;
  for (uint32_t i = 0; i < num; ++i)
  {
    out[L   * outStrideFloat + i] = in[i * N + L  ];
    out[R   * outStrideFloat + i] = in[i * N + R  ];
    out[C   * outStrideFloat + i] = in[i * N + C  ] + (in[i * N + L  ] + in[i * N + R  ]) * InvSqr2;
    out[LFE * outStrideFloat + i] = in[i * N + LFE];
    out[Ls  * outStrideFloat + i] = in[i * N + Ls ] + (in[i * N + L  ] - in[i * N + R  ]) * 0.5f;
    out[Rs  * outStrideFloat + i] = in[i * N + Rs ] + (in[i * N + R  ] - in[i * N + L  ]) * 0.5f;
  }
}

DigitalOutputContext::DeinterleaveFunc DigitalOutputContext::GetDeinterleaveFunc(const uint32_t numChannels, const bool enableUpmix)
{
  DeinterleaveFunc f = nullptr;
  switch (numChannels)
  {
    case 1: f = Deinterleave<1>; break;
    case 2: f = Deinterleave<2>; break;
    case 3: f = Deinterleave<3>; break;
    case 4: f = Deinterleave<4>; break;
    case 5: f = Deinterleave<5>; break;
    case 6: f = enableUpmix ? DeinterleaveUpmix6 : Deinterleave<6>; break;
  }
  return f;
}

double DigitalOutputContext::MeasureSafeIOCycleUsage()
{
  // we take the shortest time (which will have positive cache effects), and multiply this by SafetyFactor.
  static constexpr double SafetyFactor = 4.0;
  static constexpr uint32_t NumReps = 16;

  const auto numFramesPerPacket = GetNumFramesPerPacket();
  const auto numChannels = GetNumInputChannels();
  const auto secsPerPacket = numFramesPerPacket / GetInputFormat().mSampleRate;
  std::vector<float> input(numFramesPerPacket * numChannels, 0.0f);
  std::vector<uint8_t> output(_encoder.MaxBytesPerPacket);
  const DeinterleaveFunc deinterleaver = GetDeinterleaveFunc(numChannels, true); // measure with upmix (more processing)
  double minUsage = 1.0 / SafetyFactor; // ensures result will at most be 1.0

  for (auto i = decltype(NumReps){0}; i < NumReps; ++i)
  {
    const auto start = std::chrono::steady_clock::now();
    deinterleaver(numFramesPerPacket, input.data(), numFramesPerPacket, _planarFrames.data());
    _encoder.EncodePacket(numFramesPerPacket, _planarInputPointers.data(), static_cast<uint32_t>(output.size()), output.data());
    const auto end = std::chrono::steady_clock::now();
    const std::chrono::duration<double> duration = end - start;
    const auto usage = duration.count() / secsPerPacket;
    minUsage = std::min(minUsage, usage);
  }
  return SafetyFactor * minUsage;
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
  CAHelper::SetStreamsEnabled(_device, _deviceIOProcID, true, false); // disable any input streams

  const auto numFramesPerPacket = GetNumFramesPerPacket();
  const auto numInputChannels = GetNumInputChannels();
  _planarFrames.resize(numFramesPerPacket * numInputChannels, 0.0f);
  _planarInputPointers.resize(numInputChannels);
  for (auto i = decltype(numInputChannels){0}; i < numInputChannels; ++i)
    _planarInputPointers[i] = _planarFrames.data() + i * numFramesPerPacket;

  _deinterleaver = GetDeinterleaveFunc(numInputChannels, false);
  if (!_deinterleaver)
    throw std::runtime_error("No deinterleaver for number of input channels");

  TPCircularBufferInit(&_inputBuffer, 4 * numFramesPerPacket * numInputChannels * sizeof (float));

  const Float32 cycleUsage = static_cast<Float32>(MeasureSafeIOCycleUsage());
  static const AudioObjectPropertyAddress IOCycleUsageAddress = {kAudioDevicePropertyIOCycleUsage, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMaster};
  UInt32 dataSize = sizeof cycleUsage;
  status = AudioObjectSetPropertyData(_device, &IOCycleUsageAddress, 0, NULL, dataSize, &cycleUsage);
  if (status != noErr)
    _log.Notice("Could not set IOCycleUsage to %f", static_cast<double>(cycleUsage));
  else
    _log.Info("Set estimated IOCycleUsage to %f", static_cast<double>(cycleUsage));
}

DigitalOutputContext::~DigitalOutputContext()
{
  if (_isRunning)
    Stop();
  AudioDeviceDestroyIOProcID(_device, _deviceIOProcID);
  TPCircularBufferCleanup(&_inputBuffer);
}


void DigitalOutputContext::SetUpmix(const bool enabled)
{
  if (GetNumInputChannels() == 6)
    _deinterleaver.store(enabled ? DeinterleaveUpmix6 : Deinterleave<6>, std::memory_order_relaxed);
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
  const auto numInputChannels = me->GetNumInputChannels();
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
    const auto deinterleaver = me->_deinterleaver.load(std::memory_order_relaxed);
    deinterleaver(numFramesPerPacket, inputBuffer, numFramesPerPacket, me->_planarFrames.data());
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
