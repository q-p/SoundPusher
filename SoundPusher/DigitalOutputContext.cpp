//
//  DigitalOutputContext.cpp
//  VirtualSound
//
//  Created by Daniel Vollmer on 16/12/2015.
//
//

#include "DigitalOutputContext.hpp"

#include "SPDIFAudioEncoder.hpp"

DigitalOutputContext::DigitalOutputContext(AudioDeviceID device, AudioStreamID stream, const AudioStreamBasicDescription &format, const AudioChannelLayoutTag channelLayoutTag)
: _device(device), _stream(stream), _format(format), _channelLayoutTag(channelLayoutTag)
, _deviceIOProcID(nullptr), _isRunning(false)
, _hogger(_device, true), _mixingAllowed(_device, false), _originalFormat(_stream, format)
{
  { // pre-encode silence (in case we run out of input data)
    AudioStreamBasicDescription encoderInputFormat = {};
    encoderInputFormat.mSampleRate = format.mSampleRate;
    encoderInputFormat.mFormatID = kAudioFormatLinearPCM;
    encoderInputFormat.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    encoderInputFormat.mChannelsPerFrame = AudioChannelLayoutTag_GetNumberOfChannels(channelLayoutTag);
    // FIXME missing byte sizes etc.

    SPDIFAudioEncoder enc(encoderInputFormat, channelLayoutTag, format);

    std::vector<float> zeros(enc.GetNumFramesPerPacket(), 0.0f);
    std::vector<const float *> pointers(encoderInputFormat.mChannelsPerFrame, zeros.data());
    
    _encodedSilence.resize(enc.MaxBytesPerPacket);
    const auto silenceSize = enc.EncodePacket(static_cast<UInt32>(zeros.size()), pointers.data(), enc.MaxBytesPerPacket, _encodedSilence.data());
    _encodedSilence.resize(silenceSize);
    if (silenceSize != format.mBytesPerPacket)
      throw std::runtime_error("Encoded silence has wrong size");
  }

  OSStatus status = AudioDeviceCreateIOProcID(_device, DeviceIOProcFunc, this, &_deviceIOProcID);
  if (status != noErr)
    throw CAHelper::CoreAudioException(status);

  TPCircularBufferInit(&_packetBuffer, 3 * SPDIFAudioEncoder::MaxBytesPerPacket);
}

DigitalOutputContext::~DigitalOutputContext()
{
  if (_isRunning)
    Stop();
  TPCircularBufferCleanup(&_packetBuffer);
  AudioDeviceDestroyIOProcID(_device, _deviceIOProcID);
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
    int32_t numConsumed = me->_format.mBytesPerPacket * (availableBytes / me->_format.mBytesPerPacket);
    if (numConsumed > me->_format.mBytesPerPacket)
      printf("consumed extra packet (%i in total, one is %i)\n", numConsumed, me->_format.mBytesPerPacket);
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
