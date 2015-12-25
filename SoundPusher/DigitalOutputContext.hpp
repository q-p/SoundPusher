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
#include "CoreAudio/CoreAudio.h"
#include "TPCircularBuffer.h"

#include "MiniLogger.hpp"
#include "CoreAudioHelper.hpp"
#include "SPDIFAudioEncoder.hpp"

/// A context for a digital output device with a matching encoder.
struct DigitalOutputContext
{
  /**
   * @param device The device to use (hogged).
   * @param stream The output stream on the device to use.
   * @param format The (digital) output format to use for the stream.
   * @param channelLayoutTag The channel layout to use for both the input and compressed output data.
   */
  DigitalOutputContext(AudioObjectID device, AudioObjectID stream, const AudioStreamBasicDescription &format,
    const AudioChannelLayoutTag channelLayoutTag);

  ~DigitalOutputContext();

  /// @return the number of audio frames in a compressed packet.
  uint32_t GetNumFramesPerPacket() const { return _encoder.GetNumFramesPerPacket(); }
  /// @return the input format expected by EncodeAndAppendPacket().
  const AudioStreamBasicDescription &GetInputFormat() const { return _encoder.GetInFormat(); }

  /// Sets the pointer to the number of frames in the input buffer.
  void SetInputBufferNumFramesPointer(const std::atomic<uint32_t> *inputBufferNumFramesPointer)
  { _inputBufferNumFramesPointer = inputBufferNumFramesPointer; }

  /// Encodes the given planar input frames and appends them to the buffer for this output context.
  /**
   * Is called by a different thread, but as long as it's only one this is ok, as the buffer is thread-safe for single
   * write and consumer. The consumer is our IOProc, the producer is the caller of this function (which is the IOProc
   * of the ForwardingInputTap).
   */
  void EncodeAndAppendPacket(const uint32_t numFrames, const uint32_t numChannels, const float **inputFrames);
  /// @return the number of requested input frames to be dropped, called by both threads.
  int32_t GetNumRequestedInputFrameDrops() const { return _extraInputFrameDropRequest.load(std::memory_order_relaxed); }
  /// Adds the given number of frames to the number of requested input frames to be dropped, called by both threads.
  void AddNumRequestedInputFrameDrops(const int32_t num) { _extraInputFrameDropRequest.fetch_add(num, std::memory_order_relaxed); }

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
  /// IOProc called when the digital output device needs a new packet.
  static OSStatus DeviceIOProcFunc(AudioObjectID inDevice, const AudioTimeStamp* inNow,
    const AudioBufferList* inInputData, const AudioTimeStamp* inInputTime, AudioBufferList* outOutputData,
    const AudioTimeStamp* inOutputTime, void* inClientData);

  /// Buffer for encoded SPDIF packets (complete only).
  /**
   * Read from our IOProc and written to by EncodeAndAppendPacket() (which may be called from a different thread).
   */
  TPCircularBuffer _packetBuffer;

  /// The encoder for our output packets (not used by us except for EncodeAndAppendPacket()).
  SPDIFAudioEncoder _encoder;

  /// Pointer to the number of frames in the input buffer.
  const std::atomic<uint32_t> *_inputBufferNumFramesPointer;

  /// Counter for how many input frames we would like to have dropped.
  /**
   * This is only incremented by this thread, and only decremented by the input thread (which tries to fulfill the
   * requests)
   */
  std::atomic<int32_t> _extraInputFrameDropRequest;

  /// Counter for the approximate number of output cycles, used to monitor minimum available frames over a number of cycles.
  uint32_t _cycleCounter;

  /// The minimum number of buffered frames available at output time.
  /**
   * We want this to be >= GetNumFramesPerPacket(), but the larger we keep it (i.e. the more we buffer), the more
   * latency we incur.
   */
  uint32_t _minBufferedFramesAtOutputTime;

  /// The logger for the IOProc (which is called from a different (real-time) thread).
  MiniLogger _log;
  /// IOProc handle.
  AudioDeviceIOProcID _deviceIOProcID;

  /// Is our IOProc started?
  bool _isRunning;

  /// Hogs the output device.
  CAHelper::DeviceHogger _hogger;
  /// Sets our desired format and restores the original.
  CAHelper::FormatSetter _originalFormat;
};

#endif /* DigitalOutputContext_hpp */
