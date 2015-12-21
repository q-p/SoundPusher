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
, _inputBufferNumFrames(nullptr), _desiredInputFramesAtOutput(0), _numOutputBufferUnderruns(0), _numConsecutiveBuffers(0)
, _deviceIOProcID(nullptr), _isRunning(false)
, _hogger(_device, true), _originalFormat(_stream, format)
{
  OSStatus status = AudioDeviceCreateIOProcID(_device, DeviceIOProcFunc, this, &_deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException(status);

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
  _desiredInputFramesAtOutput = 0;
  _numOutputBufferUnderruns = 0;
  _numConsecutiveBuffers = 0;
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

  const auto numFrameIncrement = me->_format.mFramesPerPacket / 6; // how many frames to add (or less) to the input buffer
  int32_t availableBytes = 0;
  auto buffer = static_cast<uint8_t *>(TPCircularBufferTail(&me->_packetBuffer, &availableBytes));
  if (availableBytes >= me->_format.mBytesPerPacket)
  {
    uint32_t numExtraBytesToConsume = 0;
    const uint32_t numExtraPackets = (availableBytes / me->_format.mBytesPerPacket) - 1u;

    // optimize output latency by consuming excess compressed packets
    if (numExtraPackets > 0)
      numExtraBytesToConsume = numExtraPackets * me->_format.mBytesPerPacket;

    memcpy(outOutputData->mBuffers[0].mData, buffer + numExtraBytesToConsume, outOutputData->mBuffers[0].mDataByteSize);
    // note: we always consume whole packet(s), even if the request is for less
    TPCircularBufferConsume(&me->_packetBuffer, me->_format.mBytesPerPacket + numExtraBytesToConsume);

    ++me->_numConsecutiveBuffers;

    // attempt reduce extra input latency (i.e. drop some input buffer contents right now)
    const auto availableInputFrames = me->_inputBufferNumFrames->load(std::memory_order_relaxed);
    if (availableInputFrames > me->_desiredInputFramesAtOutput)
    {
//      printf("OutIOProc: %u input frames at output time, dropping %u frames\n", availableInputFrames, availableInputFrames - me->_desiredInputFramesAtOutput);
      me->_inputBufferNumFrames->store(me->_desiredInputFramesAtOutput, std::memory_order_relaxed);
      // we drop the newer frames instead of the stale ones so we don't have to copy around
    }

    // decrease desired input buffer size if we managed 2 seconds of consecutive buffers
    if (me->_desiredInputFramesAtOutput >= numFrameIncrement && me->_numConsecutiveBuffers > 2 * me->_format.mSampleRate / me->_format.mFramesPerPacket)
    {
      me->_desiredInputFramesAtOutput -= numFrameIncrement;
      printf("OutIOProc: hit %u consecutive buffers, reducing target input frames to %u\n", me->_numConsecutiveBuffers, me->_desiredInputFramesAtOutput);
      me->_numConsecutiveBuffers = 0; // let's see whether we can keep it up
    }
  }
  else
  { // provide silence
    memcpy(outOutputData->mBuffers[0].mData, me->_encodedSilence.data(), outOutputData->mBuffers[0].mDataByteSize);
    if (me->_desiredInputFramesAtOutput + numFrameIncrement < me->_format.mFramesPerPacket)
      me->_desiredInputFramesAtOutput += numFrameIncrement;
    me->_numConsecutiveBuffers = 0;
    printf("OutIOProc: %u/%u available, increasing target input frames to %u\n", availableBytes, outOutputData->mBuffers[0].mDataByteSize, me->_desiredInputFramesAtOutput);
  }
  return noErr;
}
