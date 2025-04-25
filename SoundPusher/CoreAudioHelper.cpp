//
//  CoreAudioHelper.cpp
//  SoundPusher
//
//  Created by Daniel Vollmer on 14/12/2015.
//
//

#include "CoreAudioHelper.hpp"

#include <cstring>
#include <cctype>
#include <memory>

#include <os/log.h>


namespace CAHelper {

std::string Get4CCAsString(const UInt32 val)
{
  union
  {
    UInt32 val;
    char fcc[5]; // one extra for terminating 0
  } both;

  both.val = CFSwapInt32HostToBig(val);
  // see if it appears to be a 4-char-code
  if (std::isprint(both.fcc[0]) && std::isprint(both.fcc[1]) && std::isprint(both.fcc[2]) && std::isprint(both.fcc[3]))
  {
    both.fcc[4] = '\0';
    return std::string(both.fcc);
  }
  return std::to_string(val);
}

CoreAudioException::CoreAudioException(const std::string &what, const OSStatus error)
: std::runtime_error(what + std::string(": ") + Get4CCAsString(error))
{ }

const AudioObjectPropertyAddress DeviceUIDAddress = {kAudioDevicePropertyDeviceUID, 0, 0};
const AudioObjectPropertyAddress ObjectNameAddress = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

CFStringRef GetStringProperty(const AudioObjectID device, const AudioObjectPropertyAddress &address)
{
  CFStringRef string;
  UInt32 dataSize = sizeof string;
  OSStatus status = AudioObjectGetPropertyData(device, &address, 0, NULL, &dataSize, &string);
  if (status != noErr)
    throw CAHelper::CoreAudioException("GetStringProperty(): AudioObjectGetPropertyData()", status);
  return string;
}

//==================================================================================================
#pragma mark -
#pragma mark Device queries
//==================================================================================================

std::vector<AudioStreamBasicDescription> GetStreamPhysicalFormats(const AudioObjectID stream, const Float64 desiredSampleRate)
{
  UInt32 dataSize = 0;
  OSStatus status = noErr;

  static const AudioObjectPropertyAddress physicalFormatsAddress = {kAudioStreamPropertyAvailablePhysicalFormats, kAudioObjectPropertyScopeGlobal, 0};
  // num physical formats
  status = AudioObjectGetPropertyDataSize(stream, &physicalFormatsAddress, 0, NULL, &dataSize);
  if (status != noErr)
    throw CoreAudioException("GetStreamPhysicalFormats(): AudioObjectGetPropertyDataSize()", status);

  // get physical formats
  std::vector<AudioStreamRangedDescription> rangedFormats;
  rangedFormats.resize(dataSize / sizeof rangedFormats.front());
  if (!rangedFormats.empty())
  {
    status = AudioObjectGetPropertyData(stream, &physicalFormatsAddress, 0, NULL, &dataSize, rangedFormats.data());
    if (status != noErr)
      throw CoreAudioException("GetStreamPhysicalFormats(): AudioObjectGetPropertyData()", status);
    rangedFormats.resize(dataSize / sizeof rangedFormats.front());
  }

  // throw away ranges
  std::vector<AudioStreamBasicDescription> formats;
  for (const auto &rf : rangedFormats)
  {
    // only keep if desiredSampleRate is in range
    if (desiredSampleRate != kAudioStreamAnyRate &&
      (rf.mSampleRateRange.mMinimum > desiredSampleRate || rf.mSampleRateRange.mMaximum < desiredSampleRate))
      continue;
    formats.push_back(rf.mFormat);
    if (desiredSampleRate != kAudioStreamAnyRate && rf.mFormat.mSampleRate != desiredSampleRate)
      formats.back().mSampleRate = desiredSampleRate; // then fix it (as we dropped the ranges)
  }
  formats.shrink_to_fit();

  return formats;
}


static UInt32 GetNumStreams(const AudioObjectID device, const bool input)
{
  const AudioObjectPropertyAddress streamsAddress = {kAudioDevicePropertyStreams, input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
  UInt32 dataSize = 0;
  OSStatus status = noErr;

  // num streams
  status = AudioObjectGetPropertyDataSize(device, &streamsAddress, 0, NULL, &dataSize);
  if (status != noErr)
    throw CoreAudioException("GetNumStreams(): AudioObjectGetPropertyDataSize()", status);
  return dataSize / sizeof (AudioObjectID);
}

std::vector<AudioObjectID> GetStreams(const AudioObjectID device, const bool input)
{
  const AudioObjectPropertyAddress streamsAddress = {kAudioDevicePropertyStreams, input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};

  // get streams
  std::vector<AudioObjectID> streams(GetNumStreams(device, input));
  if (!streams.empty())
  {
    UInt32 dataSize = static_cast<UInt32>(streams.size() * sizeof *streams.data());
    OSStatus status = AudioObjectGetPropertyData(device, &streamsAddress, 0, NULL, &dataSize, streams.data());
    if (status != noErr)
      throw CoreAudioException("GetStreams(): AudioObjectGetPropertyData()", status);
    streams.resize(dataSize / sizeof streams.front()); // may not have returned full buffer
  }
  return streams;
}

std::vector<AudioObjectID> GetDevices()
{
  UInt32 dataSize = 0;
  OSStatus status = noErr;

  static const AudioObjectPropertyAddress audioDevicesAddress = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

  // num devices
  status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &audioDevicesAddress, 0, NULL, &dataSize);
  if (status != noErr)
    throw CoreAudioException("GetDevices(): AudioObjectGetPropertyDataSize()", status);
  // get devices
  std::vector<AudioObjectID> devices;
  devices.resize(dataSize / sizeof devices.front());
  if (!devices.empty())
  {
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &audioDevicesAddress, 0, NULL, &dataSize, devices.data());
    if (status != noErr)
      throw CoreAudioException("GetDevices(): AudioObjectGetPropertyData()", status);
    devices.resize(dataSize / sizeof devices.front()); // may not have returned full buffer
  }
  return devices;
}

void SetStreamsEnabled(const AudioObjectID device, const AudioDeviceIOProcID IOProcID, const bool input, const bool enabled)
{
  const AudioObjectPropertyAddress streamUsageAddress = {kAudioDevicePropertyIOProcStreamUsage, input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};

  const UInt32 numStreams = GetNumStreams(device, input);

  if (numStreams == 0)
    return;

  std::size_t size = offsetof(AudioHardwareIOProcStreamUsage, mStreamIsOn) + (numStreams * sizeof(UInt32));
  auto storage = std::make_unique<std::byte[]>(size);
  auto *usage = reinterpret_cast<AudioHardwareIOProcStreamUsage *>(storage.get());
  usage->mIOProc = reinterpret_cast<void *>(IOProcID);
  usage->mNumberStreams = numStreams;
  for (UInt32 i = 0; i < numStreams; ++i)
    usage->mStreamIsOn[i] = enabled;

  OSStatus status = AudioObjectSetPropertyData(device, &streamUsageAddress, 0, NULL, static_cast<UInt32>(size), usage);
  if (status != noErr)
    os_log(OS_LOG_DEFAULT, "SetStreamsEnabled(): Could not set property: %s", Get4CCAsString(status).c_str());
}

//==================================================================================================
#pragma mark -
#pragma mark DefaultDeviceChanger
//==================================================================================================

static const AudioObjectPropertyAddress DefaultDeviceAddress = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

DefaultDeviceChanger::DefaultDeviceChanger()
: _originalDevice(kAudioObjectUnknown)
{ }

DefaultDeviceChanger::DefaultDeviceChanger(const AudioObjectID newDefaultDevice,
  DefaultDeviceChanger *oldDefaultDevice)
: _originalDevice(kAudioObjectUnknown)
{
  if (oldDefaultDevice && oldDefaultDevice->HasDevice())
    *this = std::move(*oldDefaultDevice); // transfer to us
  else
  {
    AudioObjectID defaultDevice = kAudioObjectUnknown;
    UInt32 dataSize = sizeof defaultDevice;
    const OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &DefaultDeviceAddress, 0, NULL, &dataSize, &defaultDevice);
    if (status != noErr)
      throw CoreAudioException("DefaultDeviceChanger::DefaultDeviceChanger(): AudioObjectGetPropertyData()", status);
    _originalDevice = defaultDevice;
  }

  if (_originalDevice != kAudioObjectUnknown)
  { // the default device is the one we're about to claim, so let's try to change it to the provided alternative
    UInt32 dataSize = sizeof newDefaultDevice;
    const OSStatus status = AudioObjectSetPropertyData(kAudioObjectSystemObject, &DefaultDeviceAddress, 0, NULL, dataSize, &newDefaultDevice);
    if (status != noErr)
    {
      os_log(OS_LOG_DEFAULT, "Could not change default device: %s", Get4CCAsString(status).c_str());
      return;
    }
  }
}

static void AttemptDefaultDeviceRestore(const AudioObjectID device)
{
  if (device != kAudioObjectUnknown)
  { // attempt to restore default device
    // if we do this directly, it mostly gets lost...
    dispatch_async(dispatch_get_main_queue(), ^{
      UInt32 dataSize = sizeof device;
      OSStatus status = AudioObjectSetPropertyData(kAudioObjectSystemObject, &DefaultDeviceAddress, 0, NULL, dataSize, &device);
      if (status != noErr)
        os_log(OS_LOG_DEFAULT, "Could not restore default device: %s", Get4CCAsString(status).c_str());
    });
  }
}

DefaultDeviceChanger &DefaultDeviceChanger::operator=(DefaultDeviceChanger &&other)
{
  AttemptDefaultDeviceRestore(_originalDevice);
  _originalDevice = other._originalDevice;
  other._originalDevice = kAudioObjectUnknown;
  return *this;
}

DefaultDeviceChanger::~DefaultDeviceChanger()
{
  AttemptDefaultDeviceRestore(_originalDevice);
}

//==================================================================================================
#pragma mark -
#pragma mark DeviceHogger
//==================================================================================================

static const AudioObjectPropertyAddress DeviceHogModeAddress = {kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};

DeviceHogger::DeviceHogger(const AudioObjectID device, const bool shouldHog)
: _device(device), _hog_pid(-1)
{
  if (!shouldHog)
    return;
  const auto pid = getpid();
  OSStatus status = 0;
  UInt32 dataSize = 0;

  dataSize = sizeof _hog_pid;
  status = AudioObjectGetPropertyData(_device, &DeviceHogModeAddress, 0, NULL, &dataSize, &_hog_pid);
  if (status != noErr)
    _hog_pid = -1;
  if (_hog_pid == pid)
  { // already hogged by me, so someone else will also release
    _hog_pid = -1;
    return;
  }

  // could be either a different process, or -1: either way, attempt to get it
  _hog_pid = pid;
  dataSize = sizeof _hog_pid;
  status = AudioObjectSetPropertyData(_device, &DeviceHogModeAddress, 0, NULL, dataSize, &_hog_pid);
  if (status != noErr || _hog_pid != pid)
    throw std::runtime_error("Could not obtain exclusive access to device");
}

DeviceHogger::~DeviceHogger()
{
  if (_hog_pid != getpid())
    return;
  _hog_pid = -1; // don't want to try again
  // release hog-mode
  OSStatus status = 0;
  pid_t pid = -1;
  UInt32 dataSize = sizeof pid;
  status = AudioObjectSetPropertyData(_device, &DeviceHogModeAddress, 0, NULL, dataSize, &pid);
  if (status != noErr || pid != -1)
    os_log(OS_LOG_DEFAULT, "Could not release exclusive access to device %u", _device);
}

//==================================================================================================
#pragma mark -
#pragma mark FormatSetter
//==================================================================================================

static const AudioObjectPropertyAddress StreamPhysicalFormatAddress = {kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

FormatSetter::FormatSetter(const AudioObjectID stream, const AudioStreamBasicDescription &format)
: _stream(stream), _originalFormat(), _didChange(false)
{
  OSStatus status = 0;
  UInt32 dataSize = sizeof _originalFormat;
  status = AudioObjectGetPropertyData(_stream, &StreamPhysicalFormatAddress, 0, NULL, &dataSize, &_originalFormat);
  if (status != noErr)
    throw std::runtime_error("Could not get current format for stream");

  if (std::memcmp(&_originalFormat, &format, sizeof _originalFormat) == 0)
    return;

  dataSize = sizeof format;
  status = AudioObjectSetPropertyData(_stream, &StreamPhysicalFormatAddress, 0, NULL, dataSize, &format);
  if (status != noErr)
    throw std::runtime_error("Could not set new format for stream");
  _didChange = true;
}

FormatSetter::~FormatSetter()
{
  if (!_didChange)
    return;
  _didChange = false;
  // restore original format
  OSStatus status = 0;
  UInt32 dataSize = sizeof _originalFormat;
  status = AudioObjectSetPropertyData(_stream, &StreamPhysicalFormatAddress, 0, NULL, dataSize, &_originalFormat);
  if (status != noErr)
  {
    os_log(OS_LOG_DEFAULT, "Could not restore original format on stream %u %s", _stream, Get4CCAsString(status).c_str());
    return;
  }
}

//==================================================================================================
#pragma mark -
#pragma mark DeviceBoxAcquirer
//==================================================================================================

static const AudioObjectPropertyAddress BoxForUIDAddress = {kAudioHardwarePropertyTranslateUIDToBox, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
static const AudioObjectPropertyAddress BoxAcquiredAddress = {kAudioBoxPropertyAcquired, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

DeviceBoxAcquirer::DeviceBoxAcquirer(CFStringRef boxUID)
: _box(kAudioDeviceUnknown)
{
  OSStatus status = 0;
  UInt32 dataSize = 0;

  dataSize = sizeof _box;
  status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &BoxForUIDAddress, sizeof boxUID, &boxUID, &dataSize, &_box);
  if (status != noErr)
  {
    _box = kAudioDeviceUnknown;
    os_log(OS_LOG_DEFAULT, "Could not acquire box for '%@': %s", boxUID, Get4CCAsString(status).c_str());
  }
  if (_box == kAudioDeviceUnknown)
    return;

  const UInt32 isAcquired = 1;
  dataSize = sizeof isAcquired;
  status = AudioObjectSetPropertyData(_box, &BoxAcquiredAddress, 0, NULL, dataSize, &isAcquired);
  if (status != noErr)
    throw CoreAudioException("DeviceBoxAcquirer: AudioObjectSetPropertyData(BoxAcquired) failed", status);
}

DeviceBoxAcquirer::~DeviceBoxAcquirer()
{
  if (_box == kAudioDeviceUnknown)
    return;

  OSStatus status = 0;
  UInt32 dataSize = 0;

  const UInt32 isAcquired = 0;
  dataSize = sizeof isAcquired;
  status = AudioObjectSetPropertyData(_box, &BoxAcquiredAddress, 0, NULL, dataSize, &isAcquired);
  if (status != noErr)
    os_log(OS_LOG_DEFAULT, "Could not unacquire box %u: %s", _box, Get4CCAsString(status).c_str());
}

} // end namespace
