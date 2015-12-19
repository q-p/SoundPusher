//
//  AppDelegate.mm
//  SoundPusher
//
//  Created by Daniel Vollmer on 14/12/2015.
//
//

#include <memory>
#include <algorithm>
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
} // end extern "C"

extern "C" {
#include "LoopbackAudio.h"
} // end extern "C"
#include "CoreAudioHelper.hpp"
#include "DigitalOutputContext.hpp"
#include "ForwardingInputContext.hpp"

#import "AppDelegate.h"

@interface AppDelegate ()
@property (weak) IBOutlet NSMenu *menuForStatusItem;
@end


/// Stream with compatible formats.
struct Stream
{
  AudioObjectID streamID;
  std::vector<AudioStreamBasicDescription> formats;
};

/// Device with compatible streams (that contain compatible formats).
struct Device
{
  AudioObjectID deviceID;
  std::vector<Stream> streams;
};

static std::vector<Device> GetDevicesWithDigitalOutput(const std::vector<AudioObjectID> &allDevices)
{
  std::vector<Device> result;
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
        if (format.mSampleRate < 48000.0)
          return false;
        return format.mFormatID == kAudioFormatAC3 ||
               format.mFormatID == kAudioFormat60958AC3 ||
               format.mFormatID == 'IAC3' ||
               format.mFormatID == 'iac3';

      });
      if (!digitalFormats.empty())
      {
        // sort by sample rate, so first entry will be lowest sample-rate >= 48000.0
        std::sort(digitalFormats.begin(), digitalFormats.end(), [](const AudioStreamBasicDescription &left, AudioStreamBasicDescription &right)
          { return left.mSampleRate < right.mSampleRate; });
        digitalStreams.emplace_back(Stream{stream, std::move(digitalFormats)});
      }
    }
    if (!digitalStreams.empty())
      result.emplace_back(Device{device, std::move(digitalStreams)});
  }
  return result;
}

static std::vector<Device> GetLoopbackDevicesWithInput(const std::vector<AudioObjectID> &allDevices)
{
  std::vector<Device> result;
  for (const auto device : allDevices)
  {
    NSString *uidString = CFBridgingRelease(CAHelper::GetStringProperty(device, CAHelper::DeviceUIDAddress));
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

static const AudioObjectPropertyAddress DeviceAliveAddress = {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster};

// forward declaration (because this func needs access to _chains, but _chains needs to know the type of ForwardingChain)
static OSStatus DeviceAliveListenerFunc(AudioObjectID inObjectID, UInt32 inNumberAddresses,
  const AudioObjectPropertyAddress *inAddresses, void *inClientData);

struct ForwardingChain
{
  ForwardingChain(AudioObjectID inDevice, AudioObjectID inStream,
    AudioObjectID outDevice, AudioObjectID outStream, NSInteger outStreamIndex, const AudioStreamBasicDescription &outFormat,
    const AudioChannelLayoutTag channelLayoutTag)
  : _defaultDevice(outDevice, inDevice)
  , _output(outDevice, outStream, outFormat, channelLayoutTag)
  , _input(inDevice, inStream, _output)
  , _outputDeviceUID(CFBridgingRelease(CAHelper::GetStringProperty(_output._device, CAHelper::DeviceUIDAddress))), _outputStreamIndex(outStreamIndex)
  {
      OSStatus status = AudioObjectAddPropertyListener(_output._device, &DeviceAliveAddress, DeviceAliveListenerFunc, this);
      if (status != noErr)
        NSLog(@"Could not register alive-listener for output device %u", _output._device);
      status = AudioObjectAddPropertyListener(_input._device, &DeviceAliveAddress, DeviceAliveListenerFunc, this);
      if (status != noErr)
        NSLog(@"Could not register alive-listener for input device %u", _input._device);
  }

  ~ForwardingChain()
  {
    OSStatus status = AudioObjectRemovePropertyListener(_output._device, &DeviceAliveAddress, DeviceAliveListenerFunc, this);
    if (status != noErr)
      NSLog(@"Could not remove alive-listener for output device %u", _output._device);
    status = AudioObjectRemovePropertyListener(_input._device, &DeviceAliveAddress, DeviceAliveListenerFunc, this);
    if (status != noErr)
      NSLog(@"Could not remove alive-listener for input device %u", _input._device);
  }

  CAHelper::DefaultDeviceChanger _defaultDevice;
  DigitalOutputContext _output;
  ForwardingInputContext _input;
  NSString *_outputDeviceUID;
  NSInteger _outputStreamIndex;
};

// our status menu item
NSStatusItem *_statusItem = nil;
// the list of chains which we want to be active (but which may not be, e.g. due to disconnected devices etc).
NSMutableSet<NSDictionary *> *_desiredActiveChains = [NSMutableSet new];
// the actual instances of running chains
std::vector<std::unique_ptr<ForwardingChain>> _chains;

static OSStatus DeviceAliveListenerFunc(AudioObjectID inObjectID, UInt32 inNumberAddresses,
  const AudioObjectPropertyAddress *inAddresses, void *inClientData)
{
  ForwardingChain *oldChain = static_cast<ForwardingChain *>(inClientData);
  for (auto it = _chains.begin(); it != _chains.end(); /* in body */)
  {
    const auto &chain = *it;
    if (chain.get() == oldChain)
    {
      assert(chain->_output._device == inObjectID || chain->_input._device == inObjectID);
      it = _chains.erase(it);
    }
    else
      ++it;
  }
  return noErr;
}


static void AttemptToStartMissingChains()
{
  const auto allDevices = CAHelper::GetDevices();
  const auto loopbackDevices = GetLoopbackDevicesWithInput(allDevices);
  if (loopbackDevices.empty())
    return;

  // which devices are currently available
  const auto outputDevices = GetDevicesWithDigitalOutput(allDevices);
  NSMutableSet<NSDictionary *> *desired = [NSMutableSet setWithCapacity:outputDevices.size()];
  for (const auto &outDevice : outputDevices)
  {
      NSString *outUID = CFBridgingRelease(CAHelper::GetStringProperty(outDevice.deviceID, CAHelper::DeviceUIDAddress));
      for (auto i = NSInteger{0}; i < outDevice.streams.size(); ++i)
        [desired addObject:@{@"Device" : outUID, @"Stream" : [NSNumber numberWithInteger:i]}];
  }

  // which devices are currently running
  NSMutableSet<NSDictionary *> *running = [NSMutableSet setWithCapacity:_chains.size()];
  for (const auto &chain : _chains)
    [running addObject:@{@"Device" : chain->_outputDeviceUID, @"Stream" : [NSNumber numberWithInteger:chain->_outputStreamIndex]}];

  [desired intersectSet:_desiredActiveChains];
  [desired minusSet:running];

  for (NSDictionary *attempt in desired)
  {
    NSString *uid = attempt[@"Device"];
    NSInteger streamIndex = [(NSNumber *)attempt[@"Stream"] integerValue];
    // find the device in the actual device list
    const auto outDeviceIt = std::find_if(outputDevices.cbegin(), outputDevices.cend(), [uid, streamIndex](const auto &device)
    {
      NSString *outUID = CFBridgingRelease(CAHelper::GetStringProperty(device.deviceID, CAHelper::DeviceUIDAddress));
      return streamIndex < device.streams.size() && [uid isEqualToString:outUID];
    });
    assert(outDeviceIt != outputDevices.cend());

    const auto &inDevice = loopbackDevices.front();
    const auto &outDevice = *outDeviceIt;
    const auto &outStream = outDevice.streams[streamIndex];
    try
    { // create and start-up first
      auto newChain = std::make_unique<ForwardingChain>(inDevice.deviceID, inDevice.streams.front().streamID,
        outDevice.deviceID, outStream.streamID, streamIndex, outStream.formats.front(), kAudioChannelLayoutTag_MPEG_5_1_C);
      newChain->_input.Start();
      newChain->_output.Start();

      // and only add if everything worked so far
      _chains.emplace_back(std::move(newChain));
    }
    catch (const std::exception &e)
    {
      NSLog(@"Could not initialize forwarding chain: %s", e.what());
    }
  }
}


- (void)toggleOutputDeviceAction:(NSMenuItem *)item
{
  NSString *deviceUID = item.representedObject;
  NSInteger streamIndex = item.tag;
  if (item.state == NSOnState)
  { // should try to stop chain
    bool didFind = false;
    for (auto it = _chains.begin(); it != _chains.end(); /* in body */)
    {
      const auto &chain = *it;
      if (chain->_outputStreamIndex == streamIndex && [chain->_outputDeviceUID isEqualToString:deviceUID])
      {
        didFind = true;
        it =_chains.erase(it);
      }
      else
        ++it;
    }
    // also remove from desired active
    [_desiredActiveChains removeObject:@{@"Device" : deviceUID, @"Stream" : [NSNumber numberWithInteger:streamIndex]}];
    [[NSUserDefaults standardUserDefaults] setObject:_desiredActiveChains.allObjects forKey:@"ActiveChains"];
    if (!didFind)
      NSLog(@"Could not disable chain for %@ (stream index %zd): Not found / active", deviceUID, streamIndex);
  }
  else
  { // try to add the chain
    const auto allDevices = CAHelper::GetDevices();
    const auto loopbackDevices = GetLoopbackDevicesWithInput(allDevices);
    if (loopbackDevices.empty())
    {
      NSLog(@"LoopbackAudio device is gone, cannot start chain");
      return;
    }
    const auto outputDevices = GetDevicesWithDigitalOutput(allDevices);
    for (const auto &outDevice : outputDevices)
    {
      NSString *outUID = CFBridgingRelease(CAHelper::GetStringProperty(outDevice.deviceID, CAHelper::DeviceUIDAddress));
      if (![deviceUID isEqualToString:outUID])
        continue;
      if (streamIndex >= outDevice.streams.size())
      {
        NSLog(@"Device %@ has %zu streams, but desired one is %zd", deviceUID, outDevice.streams.size(), streamIndex);
        return;
      }
      const auto &inDevice = loopbackDevices.front();
      const auto &outStream = outDevice.streams[streamIndex];
      try
      { // create and start-up first
        auto newChain = std::make_unique<ForwardingChain>(inDevice.deviceID, inDevice.streams.front().streamID,
          outDevice.deviceID, outStream.streamID, streamIndex, outStream.formats.front(), kAudioChannelLayoutTag_MPEG_5_1_C);
        newChain->_input.Start();
        newChain->_output.Start();

        // and only add if everything worked so far
        _chains.emplace_back(std::move(newChain));
        [_desiredActiveChains addObject:@{@"Device" : outUID, @"Stream" : [NSNumber numberWithInteger:streamIndex]}];
        [[NSUserDefaults standardUserDefaults] setObject:_desiredActiveChains.allObjects forKey:@"ActiveChains"];
      }
      catch (const std::exception &e)
      {
        NSLog(@"Could not initialize forwarding chain: %s", e.what());
      }
      break;
    }
  }
}


- (void)menuNeedsUpdate:(NSMenu *)menu
{
  // remove old chain menu items
  while (menu.numberOfItems > 2)
    [menu removeItemAtIndex:0];

  const auto allDevices = CAHelper::GetDevices();

  NSInteger insertionIndex = 0;
  const auto loopbackDevices = GetLoopbackDevicesWithInput(allDevices);
  if (loopbackDevices.empty())
  {
    NSMenuItem *item = [NSMenuItem new];
    item.title = @"No LoopbackAudio device";
    item.enabled = NO;
    [menu insertItem:item atIndex:insertionIndex];
    ++insertionIndex;
  }

  const auto outputDevices = GetDevicesWithDigitalOutput(allDevices);
  if (outputDevices.empty())
  {
    NSMenuItem *item = [NSMenuItem new];
    item.title = @"No (digital) output device";
    item.enabled = NO;
    [menu insertItem:item atIndex:insertionIndex];
    ++insertionIndex;
  }
  for (const auto &device : outputDevices)
  {
    NSString *deviceUID = CFBridgingRelease(CAHelper::GetStringProperty(device.deviceID, CAHelper::DeviceUIDAddress));
    NSString *deviceName = CFBridgingRelease(CAHelper::GetStringProperty(device.deviceID, CAHelper::ObjectNameAddress));

    NSInteger streamIndex = 0;
    for (const auto &stream : device.streams)
    {
      NSMenuItem *item = [NSMenuItem new];
      item.title = deviceName;
      item.enabled = !loopbackDevices.empty();
      bool isActive = false;
      for (const auto &chain : _chains)
      {
        if (chain->_output._device == device.deviceID && chain->_output._stream == stream.streamID)
        {
          isActive = true;
          break;
        }
      }
      item.state = isActive ? NSOnState : NSOffState;
      item.representedObject = deviceUID;
      item.tag = streamIndex;
      item.action = @selector(toggleOutputDeviceAction:);
      item.target = self;
      [menu insertItem:item atIndex:insertionIndex];
      ++insertionIndex;
      ++streamIndex;
    }
  }
}

- (void) receiveSleepNote: (NSNotification*) note
{
  _chains.clear();
}
 
- (void) receiveWakeNote: (NSNotification*) note
{
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
    AttemptToStartMissingChains();
  });
}
 
- (void)applicationDidFinishLaunching:(NSNotification *)aNotification
{
  // Insert code here to initialize your application

//#ifdef DEBUG
//  av_log_set_level(AV_LOG_VERBOSE);
//#else
  av_log_set_level(AV_LOG_ERROR);
//#endif
  avcodec_register_all();
  av_register_all();

  {
    NSStatusBar *bar = [NSStatusBar systemStatusBar];
    _statusItem = [bar statusItemWithLength:NSSquareStatusItemLength];
    _statusItem.button.image = [NSImage imageNamed:NSImageNameSlideshowTemplate];
    [_statusItem setMenu:self.menuForStatusItem];
  }

  NSArray<NSDictionary *> *desiredActiveArray = [[NSUserDefaults standardUserDefaults] arrayForKey:@"ActiveChains"];
  if (desiredActiveArray)
  {
    [_desiredActiveChains addObjectsFromArray:desiredActiveArray];
    AttemptToStartMissingChains();
  }

  [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:self selector:@selector(receiveSleepNote:)
    name:NSWorkspaceWillSleepNotification object:NULL];
  [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:self selector:@selector(receiveWakeNote:)
    name:NSWorkspaceDidWakeNotification object:NULL];
}

- (void)applicationWillTerminate:(NSNotification *)aNotification
{
  [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:self];

  _chains.clear();

  [_statusItem.statusBar removeStatusItem:_statusItem];
}

@end
