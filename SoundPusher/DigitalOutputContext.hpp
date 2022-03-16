//
//  DigitalOutputContext.hpp
//  SoundPusher
//
//  Created by Daniel Vollmer on 16/12/2015.
//
//

#ifndef DigitalOutputContext_hpp
#define DigitalOutputContext_hpp

#include <vector>
#include <atomic>
#include <cstdint>

#include <os/log.h>
#include "CoreAudio/CoreAudio.h"
#include "TPCircularBuffer.h"

#include "CoreAudioHelper.hpp"
#include "SPDIFAudioEncoder.hpp"

/// A context for a digital output device with a matching encoder.
struct DigitalOutputContext
{
  /// Sets the shared safetyFactor (>= 1.0) for IOCycle adjustment done during construction. IOCycle is not adjusted if < 1.0.
  static void SetIOCycleSafetyFactor(const double safetyFactor);

  /**
   * @param device The device to use (hogged).
   * @param stream The output stream on the device to use.
   * @param format The (digital) output format to use for the stream.
   * @param channelLayoutTag The channel layout to use for both the input and compressed output data.
   */
  DigitalOutputContext(AudioObjectID device, AudioObjectID stream, const AudioStreamBasicDescription &format,
    const AudioChannelLayoutTag channelLayoutTag);

  ~DigitalOutputContext();

  /// Sets the number of excess frames to keep around as a buffer.
  void SetNumSafeFrames(const uint32_t numFrames) { _numSafeFrames = numFrames; }

  /// @return the input format expected by AppendInputFrames().
  const AudioStreamBasicDescription &GetInputFormat() const { return _encoder.GetInFormat(); }
  /// @return the number of channels in (and thus required for) a compressed packet.
  uint32_t GetNumInputChannels() const { return GetInputFormat().mChannelsPerFrame; }
  /// @return the number of audio frames in (and thus required for) a compressed packet, which may be different to the frames the encoder wants.
  uint32_t GetNumFramesPerPacket() const { return _format.mFramesPerPacket; }

  /// Appends the given interleaved input frames to the internal buffer.
  /**
   * @warning Will be called from a different thread than the output thread!
   * @param numFrames The number of frames to append.
   * @param numChannels How many interleaved channels are in the source frames (must match GetNumInputChannels()).
   * @param frames Pointer to the interleaved input data.
   */
  void AppendInputFrames(const uint32_t numFrames, const uint32_t numChannels, const float *frames)
  {
    assert(numChannels == _encoder.GetInFormat().mChannelsPerFrame);
    TPCircularBufferProduceBytes(&_inputBuffer, frames, numFrames * numChannels * sizeof *frames);
  }

  /// Enables/disables upmixing from stereo if available.
  void SetUpmix(const bool enabled) { _shouldUpmix.store(enabled, std::memory_order_relaxed); }

  /// Starts IO for the device.
  void Start();
  /// Stops IO for the device.
  void Stop();

  /// The device of this context.
  const AudioObjectID _device;
  /// The stream on _device of this context.
  const AudioObjectID _stream;
  /// The digital (output) format of _stream.
  const AudioStreamBasicDescription _format;
  /// The channel layout of the digital format transported to _stream, and also expected by the encoder.
  const AudioChannelLayoutTag _channelLayoutTag;

protected:
  /// @return the over-estimated portion of how much of the output IOCycle we need to to fill the output buffer.
  double MeasureSafeIOCycleUsage(const double safetyFactor);

  /// IOProc called when the digital output device needs a new packet.
  static OSStatus DeviceIOProcFunc(AudioObjectID inDevice, const AudioTimeStamp* inNow,
    const AudioBufferList* inInputData, const AudioTimeStamp* inInputTime, AudioBufferList* outOutputData,
    const AudioTimeStamp* inOutputTime, void* inClientData);

  /// The safety-factor to use during construction.
  static double IOCycleSafetyFactor;

  /// The logger for the IOProc (which is called from a different (real-time) thread).
  os_log_t _log;

  /// The encoder for our output packets (not used by us except for EncodeAndAppendPacket()).
  SPDIFAudioEncoder _encoder;

  /// Buffer for interleaved input frames.
  TPCircularBuffer _inputBuffer;

  /// The amount of excess frames we want to keep to be sure we don't run out and have to continually readjust.
  uint32_t _numSafeFrames;
  /// Counter for the approximate number of output cycles, used to monitor minimum available frames over a number of cycles.
  uint32_t _cycleCounter;
  /// The minimum number of buffered frames available at output time.
  /**
   * We want this to be >= GetNumFramesPerPacket(), but the larger we keep it (i.e. the more we buffer), the more
   * latency we incur.
   */
  uint32_t _minBufferedFramesAtOutputTime;

  /// IOProc handle.
  AudioDeviceIOProcID _deviceIOProcID;

  /// Should we upmix?
  std::atomic_bool _shouldUpmix;

  /// Is our IOProc started?
  bool _isRunning;

  /// Hogs the output device.
  CAHelper::DeviceHogger _hogger;
  /// Sets our desired format and restores the original.
  CAHelper::FormatSetter _originalFormat;
};

#endif /* DigitalOutputContext_hpp */
