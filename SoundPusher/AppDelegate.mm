//
//  AppDelegate.mm
//  SoundPusher
//
//  Created by Daniel Vollmer on 14/12/2015.
//
//

#import "AppDelegate.h"

#include <memory>
#include <algorithm>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
} // end extern "C"

extern "C" {
#include "SoundPusherAudio.h"
} // end extern "C"
#include "CoreAudioHelper.hpp"
#include "DigitalOutputContext.hpp"
#include "ForwardingInputTap.hpp"

#import "ForwardingChainIdentifier.h"
#import "AVFoundationHelper.h"
#import "AudioTap.h"

@interface AppDelegate ()
@property (weak) IBOutlet NSMenu *statusItemMenu;
@property (weak) IBOutlet NSMenuItem *upmixMenuItem;
@end

@implementation AppDelegate

/// Stream with compatible formats.
struct Stream
{
  Stream(const AudioObjectID stream, const NSInteger streamIndexOnDevice, std::vector<AudioStreamBasicDescription> &&formats)
  : _stream(stream)
  , _streamIndexOnDevice(streamIndexOnDevice)
  , _formats(std::move(formats))
  { }

  AudioObjectID _stream;
  NSInteger _streamIndexOnDevice;
  std::vector<AudioStreamBasicDescription> _formats;
};

/// Device with compatible streams (that contain compatible formats).
struct Device
{
  Device(const AudioObjectID device, std::vector<Stream> &&streams)
  : _device(device)
  , _uid(CFBridgingRelease(CAHelper::GetStringProperty(_device, CAHelper::DeviceUIDAddress)))
  , _streams(std::move(streams))
  , _hasInputs(!CAHelper::GetStreams(_device, /* input */true).empty())
  { }

  bool Contains(const bool asInput, ForwardingChainIdentifier *identifier) const
  {
    NSString *uid = asInput ? identifier.inDeviceUID : identifier.outDeviceUID;
    NSUInteger streamIndex = asInput ? identifier.inStreamIndex : identifier.outStreamIndex;

    if (![_uid isEqualToString:uid])
      return false;
    if (streamIndex >= _streams.size())
    {
      os_log_error(OS_LOG_DEFAULT, "Device %s has %zu streams, but checked one is %zu", _uid.UTF8String, _streams.size(), streamIndex);
      return false;
    }
    return true;
  }

  AudioObjectID _device;
  NSString *_uid;
  std::vector<Stream> _streams;
  bool _hasInputs;
};

static std::vector<Device> GetDevicesWithDigitalOutput(const std::vector<AudioObjectID> &allDevices)
{
  std::vector<Device> result;
  for (const auto device : allDevices)
  {
    NSString *uid = CFBridgingRelease(CAHelper::GetStringProperty(device, CAHelper::DeviceUIDAddress));
    if ([uid hasPrefix:kAggregateDeviceUIDPrefix])
      continue;

    const auto streams = CAHelper::GetStreams(device, false /* outputs */);
    std::vector<Stream> digitalStreams;
    for (std::size_t i = 0; i < streams.size(); ++i)
    {
      const auto &stream = streams[i];
      const auto formats = CAHelper::GetStreamPhysicalFormats(stream, 48000.0);
      auto digitalFormats = decltype(formats){};
      std::copy_if(formats.begin(), formats.end(), std::back_inserter(digitalFormats), [](const AudioStreamBasicDescription &format)
      {
        assert(format.mSampleRate == 48000.0);
        return format.mFormatID == kAudioFormatAC3 ||
               format.mFormatID == kAudioFormat60958AC3 ||
               format.mFormatID == 'IAC3' ||
               format.mFormatID == 'iac3';

      });
      if (!digitalFormats.empty())
        digitalStreams.emplace_back(Stream{stream, static_cast<NSInteger>(i), std::move(digitalFormats)});
    }
    if (!digitalStreams.empty())
      result.emplace_back(Device{device, std::move(digitalStreams)});
  }
  return result;
}

static std::vector<Device> GetDevicesWith51Output(const std::vector<AudioObjectID> &allDevices)
{
  std::vector<Device> result;
  for (const auto device : allDevices)
  {
    NSString *uid = CFBridgingRelease(CAHelper::GetStringProperty(device, CAHelper::DeviceUIDAddress));
    if ([uid hasPrefix:@"de.maven.SoundPusher.Aggregate"])
      continue;

    const auto streams = CAHelper::GetStreams(device, /* inputs */false);
    std::vector<Stream> matchingStreams;
    for (std::size_t i = 0; i < streams.size(); ++i)
    {
      const auto &stream = streams[i];
      const auto formats = CAHelper::GetStreamPhysicalFormats(stream, 48000.0);
      auto acceptedOutputFormats = decltype(formats){};
      std::copy_if(formats.begin(), formats.end(), std::back_inserter(acceptedOutputFormats),
        [](const AudioStreamBasicDescription &format)
      {
        return format.mFormatID == kAudioFormatLinearPCM && format.mFramesPerPacket == 1 && format.mChannelsPerFrame == 6 && format.mFormatFlags == kAudioFormatFlagsNativeFloatPacked;
      });
      if (!acceptedOutputFormats.empty())
        matchingStreams.emplace_back(Stream{stream, static_cast<NSInteger>(i), std::move(acceptedOutputFormats)});
    }
    if (!matchingStreams.empty())
      result.emplace_back(device, std::move(matchingStreams));
  }
  return result;
}

static const AudioObjectPropertyAddress DeviceAliveAddress = {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

// forward declaration (because this func needs access to _chains, but _chains needs to know the type of ForwardingChain)
static OSStatus DeviceAliveListenerFunc(AudioObjectID inObjectID, UInt32 inNumberAddresses,
  const AudioObjectPropertyAddress *inAddresses, void *inClientData);

struct ForwardingChain
{
  ForwardingChain(ForwardingChainIdentifier *identifier, const Device &inDevice, const Stream &inStream,
    AudioObjectID outDevice, AudioObjectID outStream, const AudioStreamBasicDescription &outFormat,
    const AudioChannelLayoutTag channelLayoutTag, CAHelper::DefaultDeviceChanger *oldDefaultDevice = nullptr)
  : _identifier(identifier)
  , _defaultDevice(inDevice._device, oldDefaultDevice)
  , _output(outDevice, outStream, outFormat, channelLayoutTag,
      [[NSUserDefaults standardUserDefaults] boolForKey:@"UpmixDPLiiRear"])
  , _tappedDevice(AudioTap(inDevice._uid, inStream._streamIndexOnDevice))
  , _input(_tappedDevice._aggregateDevice, 0, _output)
  {
    OSStatus status = AudioObjectAddPropertyListener(_output._device, &DeviceAliveAddress, DeviceAliveListenerFunc, this);
    if (status != noErr)
      os_log_error(OS_LOG_DEFAULT, "Could not register alive-listener for output device %u", _output._device);
    status = AudioObjectAddPropertyListener(_input._device, &DeviceAliveAddress, DeviceAliveListenerFunc, this);
    if (status != noErr)
      os_log_error(OS_LOG_DEFAULT, "Could not register alive-listener for input device %u", _input._device);
  }

  ~ForwardingChain()
  {
    // reverse order of constructor
    OSStatus status = AudioObjectRemovePropertyListener(_input._device, &DeviceAliveAddress, DeviceAliveListenerFunc, this);
    if (status != noErr)
      os_log_error(OS_LOG_DEFAULT, "Could not remove alive-listener for input device %u", _input._device);
    status = AudioObjectRemovePropertyListener(_output._device, &DeviceAliveAddress, DeviceAliveListenerFunc, this);
    if (status != noErr)
      os_log_error(OS_LOG_DEFAULT, "Could not remove alive-listener for output device %u", _output._device);
  }

  ForwardingChainIdentifier *_identifier;
  CAHelper::DefaultDeviceChanger _defaultDevice;
  DigitalOutputContext _output;
  AggregateTappedDevice _tappedDevice;
  ForwardingInputTap _input;
};


// our status menu item
NSStatusItem *_statusItem = nil;
// the list of chains which we want to be active (but which may not be, e.g. due to disconnected devices etc).
NSMutableSet<ForwardingChainIdentifier *> *_desiredActiveChains = [NSMutableSet new];
// the actual instances of running chains
std::vector<std::unique_ptr<ForwardingChain>> _chains;
// how many menu items were in the menu originally (because we keep rebuilding parts of it)
NSInteger _numOriginalMenuItems = 0;
bool _enableUpmix = false;
AIAuthorizationStatus _audioInputAuthorizationStatus = AIAuthorizationStatusNotDetermined;


static void UpdateStatusItem()
{
  if (_chains.empty() == _statusItem.button.appearsDisabled)
    return;

  if (_chains.empty())
  {
    _statusItem.button.image = [NSImage imageNamed:@"CableCutTemplate"];
    _statusItem.button.appearsDisabled = YES;
  }
  else
  {
    _statusItem.button.image = [NSImage imageNamed:@"CableTemplate"];
    _statusItem.button.appearsDisabled = NO;
  }
}

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

  dispatch_async(dispatch_get_main_queue(), ^{
    UpdateStatusItem();
  });

  return noErr;
}


static void AttemptToStartMissingChains(CAHelper::DefaultDeviceChanger *defaultDevice = nullptr)
{
  const auto allDevices = CAHelper::GetDevices();
  const auto sourceDevices = GetDevicesWith51Output(allDevices);
  if (sourceDevices.empty())
    return;

  const auto outputDevices = GetDevicesWithDigitalOutput(allDevices);
  if (outputDevices.empty())
    return;

  NSMutableSet<NSString *> *inputDeviceUIDs = [NSMutableSet setWithCapacity:sourceDevices.size()];
  for (const auto &device : sourceDevices)
    [inputDeviceUIDs addObject:device._uid];
  NSMutableSet<NSString *> *outputDeviceUIDs = [NSMutableSet setWithCapacity:outputDevices.size()];
  for (const auto &device : outputDevices)
    [outputDeviceUIDs addObject:device._uid];


  NSMutableSet<ForwardingChainIdentifier *> *desired = [NSMutableSet set];
  for (ForwardingChainIdentifier *desiredChain in _desiredActiveChains)
  {
    if (![inputDeviceUIDs containsObject:desiredChain.inDeviceUID])
      continue;
    if (![outputDeviceUIDs containsObject:desiredChain.outDeviceUID])
      continue;
    [desired addObject:desiredChain];
  }

  // which devices are currently running
  NSMutableSet<ForwardingChainIdentifier *> *running = [NSMutableSet setWithCapacity:_chains.size()];
  for (const auto &chain : _chains)
    [running addObject:chain->_identifier];

  [desired minusSet:running];

  DigitalOutputContext::SetIOCycleSafetyFactor([[NSUserDefaults standardUserDefaults] doubleForKey:@"IOCycleSafetyFactor"]);
  for (ForwardingChainIdentifier *attempt in desired)
  {
    // find the devices in the actual device list
    const auto sourceDeviceIt = std::find_if(sourceDevices.cbegin(), sourceDevices.cend(), [attempt](const Device &device)
    {
      return device.Contains(/*asInput=*/true, attempt);
    });
    assert(sourceDeviceIt != sourceDevices.cend());

    const auto outDeviceIt = std::find_if(outputDevices.cbegin(), outputDevices.cend(), [attempt](const Device &device)
    {
      return device.Contains(/*asInput=*/false, attempt);
    });
    assert(outDeviceIt != outputDevices.cend());

    const bool requireMicAuthorization = sourceDeviceIt->_hasInputs || outDeviceIt->_hasInputs;

    if (requireMicAuthorization && _audioInputAuthorizationStatus == AIAuthorizationStatusNotDetermined)
    {
      _audioInputAuthorizationStatus = DetermineAudioInputAuthorization(^(AIAuthorizationStatus status)
      {
        dispatch_async(dispatch_get_main_queue(), ^
        {
          if (_audioInputAuthorizationStatus != status)
          {
            _audioInputAuthorizationStatus = status;
            _chains.clear();
            AttemptToStartMissingChains();
          }
        });
      });
    }

    if (requireMicAuthorization && _audioInputAuthorizationStatus != AIAuthorizationStatusAuthorized)
      continue; // other chains might use different devices

    const auto &sourceDevice = *sourceDeviceIt;
    const auto &sourceStream = sourceDevice._streams[attempt.inStreamIndex];
    const auto &outDevice = *outDeviceIt;
    const auto &outStream = outDevice._streams[attempt.outStreamIndex];
    try
    { // create and start-up first
      auto newChain = std::make_unique<ForwardingChain>(attempt, sourceDevice, sourceStream,
        outDevice._device, outStream._stream, outStream._formats.front(), kAudioChannelLayoutTag_AudioUnit_5_1,
        defaultDevice);

      newChain->_output.SetUpmix(_enableUpmix);

      newChain->_input.Start();
      newChain->_output.Start();

      // and only add if everything worked so far
      _chains.emplace_back(std::move(newChain));
    }
    catch (const std::exception &e)
    {
      os_log_error(OS_LOG_DEFAULT, "Could not initialize chain %s: %s", attempt.description.UTF8String, e.what());
    }
  }
  UpdateStatusItem();
}


- (void)toggleOutputDeviceAction:(NSMenuItem *)item
{
  ForwardingChainIdentifier *identifier = item.representedObject;
  if (item.state == NSControlStateValueOn)
  { // should try to stop chain
    bool didFind = false;
    for (auto it = _chains.begin(); it != _chains.end(); /* in body */)
    {
      const auto &chain = *it;
      if ([chain->_identifier isEqual:identifier])
      {
        didFind = true;
        it = _chains.erase(it);
      }
      else
        ++it;
    }
    // also remove from desired active
    [_desiredActiveChains removeObject:identifier];
    [[NSUserDefaults standardUserDefaults] setObject:[_desiredActiveChains.allObjects valueForKey:@"asDictionary"] forKey:@"ActiveChains"];
    if (!didFind)
      os_log_error(OS_LOG_DEFAULT, "Could not disable chain %s: Not found / active", identifier.description.UTF8String);
    else
      UpdateStatusItem();
  }
  else
  { // try to add the chain
    // need to first stop & remove all chains that use the same output device
    CAHelper::DefaultDeviceChanger defaultDevice;
    for (auto it = _chains.begin(); it != _chains.end(); /* in body */)
    {
      const auto &chain = *it;
      if ([chain->_identifier.outDeviceUID isEqual:identifier.outDeviceUID])
      {
        if (!defaultDevice.HasDevice() && (*it)->_defaultDevice.HasDevice())
          defaultDevice = std::move((*it)->_defaultDevice); // transfer the default device
        it = _chains.erase(it);
      }
      else
        ++it;
    }
    // also remove from desired active
    [_desiredActiveChains filterUsingPredicate:[NSPredicate predicateWithFormat:@"outDeviceUID != %@", identifier.outDeviceUID]];

    [_desiredActiveChains addObject:identifier];
    [[NSUserDefaults standardUserDefaults] setObject:[_desiredActiveChains.allObjects valueForKey:@"asDictionary"] forKey:@"ActiveChains"];
    AttemptToStartMissingChains(defaultDevice.HasDevice() ? &defaultDevice : nullptr);
  }
}

- (IBAction)toggleUpmix:(NSMenuItem *)item
{
  const auto newState = (item.state == NSControlStateValueOn) ? NSControlStateValueOff : NSControlStateValueOn;
  item.state = newState;
  if (_enableUpmix != (newState == NSControlStateValueOn))
  {
    _enableUpmix = (newState == NSControlStateValueOn);
    [[NSUserDefaults standardUserDefaults] setBool:_enableUpmix forKey:@"Upmix"];

    for (const auto &chain : _chains)
      chain->_output.SetUpmix(_enableUpmix);
  }
}


- (void)menuNeedsUpdate:(NSMenu *)menu
{
  bool showDebugInfo = ([NSEvent modifierFlags] & NSEventModifierFlagOption) != 0;
  // remove old chain menu items
  while (menu.numberOfItems > _numOriginalMenuItems)
    [menu removeItemAtIndex:0];

  const auto allDevices = CAHelper::GetDevices();

  const bool isAudioInputUnavailable = _audioInputAuthorizationStatus == AIAuthorizationStatusRestricted || _audioInputAuthorizationStatus == AIAuthorizationStatusDenied;

  NSUInteger insertionIndex = 0;
  const auto sourceDevices = GetDevicesWith51Output(allDevices);
  const auto numSourcesThatRequireAudioInput = std::count_if(sourceDevices.cbegin(), sourceDevices.cend(), [](const Device &device)
  {
    return device._hasInputs;
  });
  if (sourceDevices.empty())
  {
    NSMenuItem *item = [NSMenuItem new];
    item.title = @"No (suitable) 6-channel source device (e.g. SoundPusher Audio)";
    item.enabled = NO;
    [menu insertItem:item atIndex:insertionIndex];
    ++insertionIndex;
  }
  else if (numSourcesThatRequireAudioInput > 0 && isAudioInputUnavailable)
  {
    NSMenuItem *item = [NSMenuItem new];
    switch (_audioInputAuthorizationStatus)
    {
      case AIAuthorizationStatusRestricted:
        item.title = @"Access to audio input (microphone) is restricted";
      break;
      case AIAuthorizationStatusDenied:
        item.title = @"Access to audio input (microphone) denied";
      break;
      default: assert(false);
    }
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

  for (const auto &sourceDevice : sourceDevices)
  {
    NSString *sourceDeviceName = CFBridgingRelease(CAHelper::GetStringProperty(sourceDevice._device, CAHelper::ObjectNameAddress));

    for (auto inStreamIdx = NSUInteger{0}; inStreamIdx < sourceDevice._streams.size(); ++inStreamIdx)
    {
      for (const auto &outDevice : outputDevices)
      {
        NSString *outDeviceName = CFBridgingRelease(CAHelper::GetStringProperty(outDevice._device, CAHelper::ObjectNameAddress));

        for (auto outStreamIdx = NSUInteger{0}; outStreamIdx < outDevice._streams.size(); ++outStreamIdx)
        {
          ForwardingChainIdentifier *identifier = [[ForwardingChainIdentifier alloc] initWithInDeviceUID:sourceDevice._uid andInStreamIndex:inStreamIdx andOutDeviceUID:outDevice._uid andOutStreamIndex:outStreamIdx];
          const AudioStreamBasicDescription &outFormat = outDevice._streams[outStreamIdx]._formats.front();
          NSMenuItem *item = [NSMenuItem new];
          if (sourceDevice._streams.size() > 1 || outDevice._streams.size() > 1)
            item.title = [NSString stringWithFormat:@"%@ (%lu) → %@ (%lu)", sourceDeviceName, inStreamIdx, outDeviceName, outStreamIdx];
          else
            item.title = [NSString stringWithFormat:@"%@ → %@", sourceDeviceName, outDeviceName];

          item.enabled = !(sourceDevices.empty() || (sourceDevice._hasInputs && isAudioInputUnavailable)); // either allowed or undetermined
          const auto chainIt = std::find_if(_chains.cbegin(), _chains.cend(), [identifier](const std::unique_ptr<ForwardingChain> &chain)
          {
            return [identifier isEqual:chain->_identifier];
          });
          const bool isActive = chainIt != _chains.cend();
          item.state = isActive ? NSControlStateValueOn : NSControlStateValueOff;
          item.representedObject = identifier;
          item.action = @selector(toggleOutputDeviceAction:);
          item.target = self;
          [menu insertItem:item atIndex:insertionIndex];
          ++insertionIndex;
          if (showDebugInfo)
          {
            NSMenuItem *item = [NSMenuItem new];
            item.title = [NSString stringWithFormat:@"%.0fHz %u bytes/packet %u frames/packet [%s]", outFormat.mSampleRate, outFormat.mBytesPerPacket, outFormat.mFramesPerPacket, CAHelper::Get4CCAsString(outFormat.mFormatID).c_str()];
            item.enabled = NO;
            [menu insertItem:item atIndex:insertionIndex];
            ++insertionIndex;
          }
        }
      }
    }
  }
}

- (IBAction)openHomepage:(id)sender
{
  [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://github.com/q-p/SoundPusher"]];
}

- (void) receiveSleepNote: (NSNotification*) note
{
  _chains.clear();
}
 
- (void) receiveWakeNote: (NSNotification*) note
{
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    AttemptToStartMissingChains();
  });
}
 
- (void)applicationDidFinishLaunching:(NSNotification *)aNotification
{
  // register defaults
  [[NSUserDefaults standardUserDefaults] registerDefaults:@{
    @"Upmix" : [NSNumber numberWithBool:NO],
    @"UpmixDPLiiRear" : [NSNumber numberWithBool:YES],
    @"IOCycleSafetyFactor" : [NSNumber numberWithDouble:8.0],
    @"ActiveChains" : @[]
  }];

  { // read defaults
    _enableUpmix = [[NSUserDefaults standardUserDefaults] boolForKey:@"Upmix"];
    // DigitalOutputContext::IOCycleSafetyFactor is set in #AttemptToStartMissingChains()

    NSArray<NSDictionary *> *desiredActiveArray = [[NSUserDefaults standardUserDefaults] arrayForKey:@"ActiveChains"];
    for (NSDictionary *dict in desiredActiveArray)
    {
      if (![dict isKindOfClass:NSDictionary.class])
        continue;
      ForwardingChainIdentifier *identifier = [ForwardingChainIdentifier identifierWithDictionary:dict];
      if (!identifier)
        continue;
      [_desiredActiveChains addObject:identifier];
    }
  }

  _numOriginalMenuItems = self.statusItemMenu.numberOfItems; // so we know how many to keep when updating the menu

  _upmixMenuItem.state = _enableUpmix ? NSControlStateValueOn : NSControlStateValueOff;

#ifdef DEBUG
  av_log_set_level(AV_LOG_VERBOSE);
#else
  av_log_set_level(AV_LOG_ERROR);
#endif

  {
    NSStatusBar *bar = [NSStatusBar systemStatusBar];
    _statusItem = [bar statusItemWithLength:NSSquareStatusItemLength];
    _statusItem.button.image = [NSImage imageNamed:@"CableCutTemplate"];
    _statusItem.button.appearsDisabled = YES;
    [_statusItem setMenu:self.statusItemMenu];
  }

  _audioInputAuthorizationStatus = GetAudioInputAuthorizationStatus();

  AttemptToStartMissingChains();

  [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:self selector:@selector(receiveSleepNote:)
    name:NSWorkspaceWillSleepNotification object:NULL];
  [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:self selector:@selector(receiveWakeNote:)
    name:NSWorkspaceDidWakeNotification object:NULL];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
  // we always want to quit *BUT* we need any events posted by deleting the chains (such as restoring the default
  // device) to finish processing

  // delete the chains (this posts some events)
  _chains.clear();
  // now we add an event *after* those that actually quits after the events have been processed
  dispatch_async(dispatch_get_main_queue(), ^{
    [[NSApplication sharedApplication] replyToApplicationShouldTerminate:YES];
  });
  return NSTerminateLater;
}

- (void)applicationWillTerminate:(NSNotification *)aNotification
{
  [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:self];
  [_statusItem.statusBar removeStatusItem:_statusItem];
}

@end
