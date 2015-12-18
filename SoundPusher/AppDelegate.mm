//
//  AppDelegate.mm
//  SoundPusher
//
//  Created by Daniel Vollmer on 14/12/2015.
//
//

#include <memory>

extern "C" {
#include "LoopbackAudio.h"

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
} // end extern "C"

#include "CoreAudioHelper.hpp"
#include "DigitalOutputContext.hpp"
#include "ForwardingInputContext.hpp"

#import "AppDelegate.h"

@interface AppDelegate ()
@property (weak) IBOutlet NSWindow *window;
@end


struct Stream
{
  AudioStreamID streamID;
  std::vector<AudioStreamBasicDescription> formats;
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
    const auto streams = CAHelper::GetStreams(device, false /* outputs */);
    std::vector<Stream> digitalStreams;
    for (const auto stream : streams)
    {
      const auto formats = CAHelper::GetStreamPhysicalFormats(stream);
      auto digitalFormats = decltype(formats){};
      std::copy_if(formats.begin(), formats.end(), std::back_inserter(digitalFormats), [](const AudioStreamBasicDescription &format)
      {
        return format.mFormatID == kAudioFormatAC3 ||
               format.mFormatID == kAudioFormat60958AC3 ||
               format.mFormatID == 'IAC3' ||
               format.mFormatID == 'iac3';

      });
      if (!digitalFormats.empty())
        digitalStreams.emplace_back(Stream{stream, std::move(digitalFormats)});
    }
    if (!digitalStreams.empty())
      result.emplace_back(Device{device, std::move(digitalStreams)});
  }
  return result;
}

static std::vector<Device> GetLoopbackDevicesWithInput()
{
  std::vector<Device> result;
  const auto allDevices = CAHelper::GetDevices();
  for (const auto device : allDevices)
  {
    OSStatus status = 0;
    NSString *uidString;
    UInt32 dataSize = sizeof uidString;

    const AudioObjectPropertyAddress DeviceUIDAddress = { kAudioDevicePropertyDeviceUID, 0, 0 };
    status = AudioObjectGetPropertyData(device, &DeviceUIDAddress, 0, NULL, &dataSize, &uidString);
    if (status != noErr)
      throw CAHelper::CoreAudioException(status);
    if (![uidString isEqualToString:[NSString stringWithUTF8String:kLoopbackAudioDevice_UID]])
      continue;

    const auto streams = CAHelper::GetStreams(device, true /* inputs */);
    std::vector<Stream> matchingStreams;
    for (const auto stream : streams)
    {
      auto formats = CAHelper::GetStreamPhysicalFormats(stream);
      if (!formats.empty())
        matchingStreams.emplace_back(Stream{stream, std::move(formats)});
    }
    if (!matchingStreams.empty())
      result.emplace_back(Device{device, std::move(matchingStreams)});
  }
  return result;
}

@implementation AppDelegate

std::unique_ptr<DigitalOutputContext> _output;
std::unique_ptr<ForwardingInputContext> _input;

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification
{
  // Insert code here to initialize your application

  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^
  {
    av_log_set_level(AV_LOG_VERBOSE);
//    av_log_set_level(AV_LOG_ERROR);
    avcodec_register_all();
    av_register_all();
  });

  // look for compatible output devices

  // look for LoopbackAudio

  // check preferred output format (codec, number of channels, sample-rate, bit-rate, options)

  // set best-matching options on LoopbackAudio's *input* (so it reconfigures its *output* (source) format)

  // if desired, set system default device to LoopbackAudio (remembering old device)

  // set-up *real* output (hog, etc.)
  { // set up output
    const auto devices = GetDevicesWithDigitalOutput();
    for (const auto &device : devices)
    {
      NSLog(@"Device: %u", device.deviceID);
      for (const auto &stream : device.streams)
      {
        NSLog(@"  Stream: %u", stream.streamID);
        for (const auto &format : stream.formats)
        {
//          AVAudioFormat *avFormat = [[AVAudioFormat alloc] initWithStreamDescription:&format];
//          NSLog(@"    Format: %@", avFormat);
        }
      }
    }

    const auto &device = devices.front();
    const auto &stream = device.streams.front();
    const auto &format = stream.formats.at(1);
    try
    {
      _output = std::make_unique<DigitalOutputContext>(device.deviceID, stream.streamID, format, kAudioChannelLayoutTag_MPEG_5_1_C);
    }
    catch (const std::exception &e)
    {
//      NSLog(@"Could not initialize output context for device %u stream %u format %@", device.deviceID, stream.streamID, [[AVAudioFormat alloc] initWithStreamDescription:&format]);
    }
  }
  if (!_output)
    return;
//  NSLog(@"Output context initialized for device %u stream %u format %@", _output->_device, _output->_stream, [[AVAudioFormat alloc] initWithStreamDescription:&_output->_format]);


  { // set up output
    const auto devices = GetLoopbackDevicesWithInput();

    const auto &device = devices.front();
    const auto &stream = device.streams.front();
    try
    {
      _input = std::make_unique<ForwardingInputContext>(device.deviceID, stream.streamID, *_output);
    }
    catch (const std::exception &e)
    {
      NSLog(@"Could not initialize input context for device %u stream %u", device.deviceID, stream.streamID);
    }
  }
  if (!_input)
    return;

  // install tap on LoopbackAudio (filling Input)
  _input->Start();
  // and now try to produce output
  _output->Start();
}

- (void)applicationWillTerminate:(NSNotification *)aNotification
{
  // Insert code here to tear down your application

  if (_output)
    _output->Stop();
  if (_input)
    _input->Stop();

  // delete input device first (as it keeps a reference to the output device)
  _input.reset();
  // delete output device
  _output.reset();
}

@end
