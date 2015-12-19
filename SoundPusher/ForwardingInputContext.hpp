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
#include "CoreAudio/CoreAudio.h"

#include "CoreAudioHelper.hpp"

// forward declaration
struct DigitalOutputContext;

/// Takes input from device.stream and forwards it to the outContext as soon as a full packet has been buffered.
struct ForwardingInputContext
{
  /**
   * @param device The device whose data to forward data to the outContext.
   * @param stream The input stream on device to forward.
   * @param outContext The output context to forward data to. Must outlive us.
   */
  ForwardingInputContext(AudioObjectID device, AudioObjectID stream, DigitalOutputContext &outContext);

  ~ForwardingInputContext();


  /// Starts IO for the device.
  void Start();
  /// Stops IO for the device.
  void Stop();

  /// The device of this context.
  const AudioObjectID _device;
  /// The input stream on _device of this context.
  const AudioObjectID _stream;
  /// The input format read from _stream.
  const AudioStreamBasicDescription _format;

protected:
  /// IOProc called when the input device has new frames for us.
  static OSStatus DeviceIOProcFunc(AudioObjectID inDevice, const AudioTimeStamp* inNow,
    const AudioBufferList* inInputData, const AudioTimeStamp* inInputTime, AudioBufferList* outOutputData,
    const AudioTimeStamp* inOutputTime, void* inClientData);

  /// The output context to which we send any received data.
  DigitalOutputContext &_outContext;

  /// These point back into the correct offsets into _planarFrames
  std::vector<const float *> _planarInputPointers;
  /// The number of frames currently buffered.
  uint32_t _numBufferedFrames;

  /// Backing storage for all planes.
  std::vector<float> _planarFrames;

  /// IOProc handle.
  AudioDeviceIOProcID _deviceIOProcID;

  /// Is our IOProc started?
  bool _isRunning;
};

#endif /* ForwardingInputContext_hpp */
