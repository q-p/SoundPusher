//
//  ForwardingInputContext.hpp
//  VirtualSound
//
//  Created by Daniel Vollmer on 16/12/2015.
//
//

#ifndef ForwardingInputContext_hpp
#define ForwardingInputContext_hpp

#include <vector>

#include "CoreAudioHelper.hpp"
#include "SPDIFAudioEncoder.hpp"

// forward declaration
struct DigitalOutputContext;

struct ForwardingInputContext
{
  ForwardingInputContext(AudioDeviceID device, AudioStreamID stream, DigitalOutputContext &outContext);
  ~ForwardingInputContext();

  void Start();
  void Stop();

  const AudioDeviceID _device;
  const AudioStreamID _stream;
  const AudioStreamBasicDescription _format;

protected:
  static OSStatus DeviceIOProcFunc(AudioObjectID inDevice, const AudioTimeStamp* inNow,
    const AudioBufferList* inInputData, const AudioTimeStamp* inInputTime, AudioBufferList* outOutputData,
    const AudioTimeStamp* inOutputTime, void* inClientData);

  DigitalOutputContext &_outContext;

  SPDIFAudioEncoder _encoder;

  std::vector<float> _planarFrames;
  /// These point back into the correct offsets into _planarFrames
  std::vector<const float *> _planarInputPointers;
  uint32_t _numBufferedFrames;

  AudioDeviceIOProcID _deviceIOProcID;

  bool _isRunning;

//  CAHelper::FormatSetter _originalFormat;
};

#endif /* ForwardingInputContext_hpp */
