//
//  AppDelegate.mm
//  SoundPusher
//
//  Created by Daniel Vollmer on 14/12/2015.
//
//

#include <memory>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
} // end extern "C"

#include "CoreAudioHelper.hpp"
#include "SPDIFAudioEncoder.hpp"


#import "AppDelegate.h"
#import "AVFoundation/AVFoundation.h"

@interface AppDelegate ()
@property (weak) IBOutlet NSWindow *window;
@end


struct Stream
{
  AudioStreamID streamID;
  std::vector<AudioStreamRangedDescription> formats;
};

struct Device
{
  AudioDeviceID deviceID;
  std::vector<Stream> streams;
};

static std::vector<Device> GetDevicesWithDigitalOutput()
{
  std::vector<Device> result;
  const auto allDevices = CAHelper::GetDevices();
  for (const auto device : allDevices)
  {
    const auto streams = CAHelper::GetStreams(device, false);
    std::vector<Stream> digitalStreams;
    for (const auto stream : streams)
    {
      const auto formats = CAHelper::GetStreamPhysicalFormats(stream);
      auto digitalFormats = decltype(formats){};
      std::copy_if(formats.begin(), formats.end(), std::back_inserter(digitalFormats), [](const AudioStreamRangedDescription &format)
      {
        return format.mFormat.mFormatID == kAudioFormatAC3 ||
               format.mFormat.mFormatID == kAudioFormat60958AC3 ||
               format.mFormat.mFormatID == 'IAC3' ||
               format.mFormat.mFormatID == 'iac3';

      });
      if (!digitalFormats.empty())
        digitalStreams.emplace_back(Stream{stream, std::move(digitalFormats)});
    }
    if (!digitalStreams.empty())
      result.emplace_back(Device{device, std::move(digitalStreams)});
  }
  return result;
}

static std::vector<Device> Test()
{
  std::vector<Device> result;
  const auto allDevices = CAHelper::GetDevices();
  for (const auto device : allDevices)
  {
    OSStatus status = 0;
    CFStringRef uidString;
    UInt32 dataSize = sizeof uidString;

    const AudioObjectPropertyAddress DeviceUIDAddress = { kAudioDevicePropertyDeviceUID, 0, 0 };
    status = AudioObjectGetPropertyData(device, &DeviceUIDAddress, 0, NULL, &dataSize, &uidString);
    if (status != noErr)
      throw CAHelper::CoreAudioException(status);
    NSLog(@"DeviceUID %@", uidString);
  }
  return result;
}

@implementation AppDelegate

AVAudioEngine *_audioEngine = nil;

struct DeviceContext
{
  DeviceContext(AudioDeviceID device, AudioStreamID stream, const AudioStreamRangedDescription &format)
  : _device(device), _stream(stream), _format(format)
//  , _hogger(_device), _disabledMixer(_device, false), _originalFormat(_stream, format.mFormat)
  { }

  AudioDeviceID _device;
  AudioStreamID _stream;
  AudioStreamRangedDescription _format;
//protected:
//  CAHelper::DeviceHogger _hogger;
//  CAHelper::MixingSetter _disabledMixer;
//  CAHelper::FormatSetter _originalFormat;
};

std::unique_ptr<DeviceContext> _context;

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification
{
  // Insert code here to initialize your application

  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^
  {
    avcodec_register_all();
    av_register_all();
  });

  const auto devices = GetDevicesWithDigitalOutput();
  for (const auto &device : devices)
  {
    NSLog(@"Device: %u", device.deviceID);
    for (const auto &stream : device.streams)
    {
      NSLog(@"  Stream: %u", stream.streamID);
      for (const auto &format : stream.formats)
      {
        AVAudioFormat *avFormat = [[AVAudioFormat alloc] initWithStreamDescription:&format.mFormat];
        NSLog(@"    Format: %@ (%f - %f)", avFormat, format.mSampleRateRange.mMinimum, format.mSampleRateRange.mMaximum);
      }
    }
  }

  const auto &device = devices.front();
  const auto &stream = device.streams.front();
  const auto &format = stream.formats.at(1);

  try
  {
    _context = std::make_unique<DeviceContext>(device.deviceID, stream.streamID, format);
  }
  catch (const std::exception &e)
  {
    NSLog(@"Could not initialize Device %u Stream %u Format %@", device.deviceID, stream.streamID, [[AVAudioFormat alloc] initWithStreamDescription:&format.mFormat]);
  }

  if (!_context)
    return;

  NSLog(@"Context initialized for Device %u Stream %u Format %@", _context->_device, _context->_stream, [[AVAudioFormat alloc] initWithStreamDescription:&_context->_format.mFormat]);

  Test();

  {
    SPDIFAudioEncoder test(_context->_format.mFormat, kAudioChannelLayoutTag_MPEG_5_1_C, _context->_format.mFormat);

    float zero[1536] = {};
    const float *frames[6] = {zero, zero, zero, zero, zero, zero};
    uint8_t spdif[6144];

    test.EncodePacket(1536, frames, sizeof spdif, spdif);
    test.EncodePacket(1536, frames, sizeof spdif, spdif);
  }

//  OSStatus status = 0;
//  _audioEngine = [AVAudioEngine new];
//  AudioUnit outputUnit = _audioEngine.outputNode.audioUnit;
//  status = AudioUnitSetProperty(outputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &_context->_device, sizeof _context->_device);
//  if (status != noErr)
//    throw CAHelper::CoreAudioException(status);
//
//  AudioStreamBasicDescription asbd;
//  UInt32 dataSize = sizeof asbd;
//  status = AudioUnitGetProperty(outputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &asbd, &dataSize);
//  if (status != noErr || dataSize != sizeof asbd)
//    throw CAHelper::CoreAudioException(status);
//
//  NSLog(@"AudioUnit Format %@", [[AVAudioFormat alloc] initWithStreamDescription:&asbd]);
}

- (void)applicationWillTerminate:(NSNotification *)aNotification
{
  // Insert code here to tear down your application

  // release context
  _context = nullptr;
}

@end
