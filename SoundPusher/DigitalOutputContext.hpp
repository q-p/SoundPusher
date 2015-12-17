//
//  DigitalOutputContext.hpp
//  VirtualSound
//
//  Created by Daniel Vollmer on 16/12/2015.
//
//

#ifndef DigitalOutputContext_hpp
#define DigitalOutputContext_hpp

#include <vector>

#include "CoreAudio/CoreAudio.h"

#include "CoreAudioHelper.hpp"
#include "TPCircularBuffer.h"

struct DigitalOutputContext
{
  DigitalOutputContext(AudioDeviceID device, AudioStreamID stream, const AudioStreamBasicDescription &format,
    const AudioChannelLayoutTag channelLayoutTag);
  ~DigitalOutputContext();

  TPCircularBuffer *GetPacketBuffer() { return &_packetBuffer; }

  void Start();
  void Stop();

  const AudioDeviceID _device;
  const AudioStreamID _stream;
  const AudioStreamBasicDescription _format;
  const AudioChannelLayoutTag _channelLayoutTag;

protected:
  static OSStatus DeviceIOProcFunc(AudioObjectID inDevice, const AudioTimeStamp* inNow,
    const AudioBufferList* inInputData, const AudioTimeStamp* inInputTime, AudioBufferList* outOutputData,
    const AudioTimeStamp* inOutputTime, void* inClientData);

  /// buffer for encoded packets (complete only)
  // CHECKME: what do we do with old-ish packets? how many do we want to keep around?
  // CHECKME: single producer, single consumer, so producer cannot remove excess
  TPCircularBuffer _packetBuffer;

  std::vector<uint8_t> _encodedSilence;

  AudioDeviceIOProcID _deviceIOProcID;

  bool _isRunning;

  CAHelper::DeviceHogger _hogger;
  CAHelper::MixingSetter _mixingAllowed;
  CAHelper::FormatSetter _originalFormat;
};

#endif /* DigitalOutputContext_hpp */
