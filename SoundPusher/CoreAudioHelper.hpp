//
//  CoreAudioHelper.hpp
//  VirtualSound
//
//  Created by Daniel Vollmer on 14/12/2015.
//
//

#ifndef CoreAudioHelper_hpp
#define CoreAudioHelper_hpp

#include <vector>
#include <stdexcept>
#include "CoreAudio/CoreAudio.h"

namespace CAHelper {

struct CoreAudioException : std::runtime_error { CoreAudioException(const OSStatus error); };

std::vector<AudioStreamBasicDescription> GetStreamPhysicalFormats(const AudioObjectID stream);
std::vector<AudioObjectID> GetStreams(const AudioObjectID device, const bool input);
std::vector<AudioObjectID> GetDevices();

/// RAII class for hogging a device
struct DeviceHogger
{
  DeviceHogger(const AudioObjectID device, const bool shouldHog);
  DeviceHogger(DeviceHogger &&) = delete; // non-copyable, non-movable
  ~DeviceHogger();
protected:
  AudioObjectID _device;
  pid_t _hog_pid;
};

/// RAII class for setting the mixing state on a device (and restoring the original state on destruction)
struct MixingSetter
{
  MixingSetter(const AudioObjectID device, const bool supportMixing);
  MixingSetter(MixingSetter &&) = delete; // non-copyable, non-movable
  ~MixingSetter();
protected:
  AudioObjectID _device;
  UInt32 _originalState;
  bool _didChange;
};

/// RAII class for setting a stream format (and restoring the original format on destruction)
struct FormatSetter
{
  FormatSetter(const AudioObjectID stream, const AudioStreamBasicDescription &format);
  FormatSetter(FormatSetter &&) = delete; // non-copyable, non-movable
  ~FormatSetter();
protected:
  AudioObjectID _stream;
  AudioStreamBasicDescription _originalFormat;
  bool _didChange;
};


} // end namespace CAHelper

#endif /* CoreAudioHelper_hpp */
