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
#include "SoundPusherAudio.h"
} // end extern "C"
#include "CoreAudioHelper.hpp"
#include "DigitalOutputContext.hpp"
#include "ForwardingInputTap.hpp"

#import "ForwardingChainIdentifier.h"
#import "AppDelegate.h"
#import "AVFoundationHelper.h"

@interface AppDelegate ()
@property (weak) IBOutlet NSMenu *statusItemMenu;
@property (weak) IBOutlet NSMenuItem *upmixMenuItem;
@end

@implementation AppDelegate

/// Stream with compatible formats.
struct Stream
{
  Stream(const AudioObjectID stream, std::vector<AudioStreamBasicDescription> &&formats)
  : _stream(stream)
  , _formats(std::move(formats))
  { }

  AudioObjectID _stream;
  std::vector<AudioStreamBasicDescription> _formats;
};

/// Device with compatible streams (that contain compatible formats).
struct Device
{
  Device(const AudioObjectID device, std::vector<Stream> &&streams)
  : _device(device)
  , _uid(CFBridgingRelease(CAHelper::GetStringProperty(_device, CAHelper::DeviceUIDAddress)))
  , _streams(std::move(streams))
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
        digitalStreams.emplace_back(Stream{stream, std::move(digitalFormats)});
    }
    if (!digitalStreams.empty())
      result.emplace_back(Device{device, std::move(digitalStreams)});
  }
  return result;
}

static std::vector<Device> GetDevicesWithMatchingInput(const std::vector<AudioObjectID> &allDevices)
{
  std::vector<Device> result;
  for (const auto device : allDevices)
  {
//    NSString *uidString = CFBridgingRelease(CAHelper::GetStringProperty(device, CAHelper::DeviceUIDAddress));
//    if (![uidString isEqualToString:[NSString stringWithUTF8String:kDevice_UID]])
//      continue;

    const auto streams = CAHelper::GetStreams(device, true /* inputs */);
    std::vector<Stream> matchingStreams;
    for (const auto stream : streams)
    {
      const auto formats = CAHelper::GetStreamPhysicalFormats(stream, 48000.0);
      auto acceptedInputFormats = decltype(formats){};
      std::copy_if(formats.begin(), formats.end(), std::back_inserter(acceptedInputFormats),
        [](const AudioStreamBasicDescription &format)
      {
        return format.mFormatID == kAudioFormatLinearPCM && format.mFramesPerPacket == 1 && format.mChannelsPerFrame == 6 && format.mFormatFlags == kAudioFormatFlagsNativeFloatPacked;
      });
      if (!acceptedInputFormats.empty())
        matchingStreams.emplace_back(Stream{stream, std::move(acceptedInputFormats)});
    }
    if (!matchingStreams.empty())
      result.emplace_back(Device{device, std::move(matchingStreams)});
  }
  return result;
}

static const AudioObjectPropertyAddress DeviceAliveAddress = {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster};

// forward declaration (because this func needs access to _chains, but _chains needs to know the type of ForwardingChain)
static OSStatus DeviceAliveListenerFunc(AudioObjectID inObjectID, UInt32 inNumberAddresses,
  const AudioObjectPropertyAddress *inAddresses, void *inClientData);

struct ForwardingChain
{
  ForwardingChain(ForwardingChainIdentifier *identifier, AudioObjectID inDevice, AudioObjectID inStream,
    AudioObjectID outDevice, AudioObjectID outStream, const AudioStreamBasicDescription &outFormat,
    const AudioChannelLayoutTag channelLayoutTag)
  : _identifier(identifier)
  , _defaultDevice(outDevice, inDevice)
  , _output(outDevice, outStream, outFormat, channelLayoutTag)
  , _input(inDevice, inStream, _output)
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


static void AttemptToStartMissingChains()
{
  const auto allDevices = CAHelper::GetDevices();
  const auto inputDevices = GetDevicesWithMatchingInput(allDevices);
  if (inputDevices.empty())
    return;

  if (_audioInputAuthorizationStatus == AIAuthorizationStatusRestricted || _audioInputAuthorizationStatus == AIAuthorizationStatusDenied)
    return;

  const auto outputDevices = GetDevicesWithDigitalOutput(allDevices);
  if (outputDevices.empty())
    return;

  NSMutableSet<NSString *> *inputDeviceUIDs = [NSMutableSet setWithCapacity:inputDevices.size()];
  for (const auto &device : inputDevices)
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

  for (ForwardingChainIdentifier *attempt in desired)
  {
    // find the devices in the actual device list
    const auto inDeviceIt = std::find_if(inputDevices.cbegin(), inputDevices.cend(), [attempt](const Device &device)
    {
      return device.Contains(/*asInput=*/true, attempt);
    });
    assert(inDeviceIt != inputDevices.cend());

    const auto outDeviceIt = std::find_if(outputDevices.cbegin(), outputDevices.cend(), [attempt](const Device &device)
    {
      return device.Contains(/*asInput=*/false, attempt);
    });
    assert(outDeviceIt != outputDevices.cend());

    if (_audioInputAuthorizationStatus == AIAuthorizationStatusNotDetermined)
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

    if (_audioInputAuthorizationStatus != AIAuthorizationStatusAuthorized)
      return; // will also fail for any other chain

    const auto &inDevice = *inDeviceIt;
    const auto &inStream = inDevice._streams[attempt.inStreamIndex];
    const auto &outDevice = *outDeviceIt;
    const auto &outStream = outDevice._streams[attempt.outStreamIndex];
    try
    { // create and start-up first
      auto newChain = std::make_unique<ForwardingChain>(attempt, inDevice._device, inStream._stream,
        outDevice._device, outStream._stream, outStream._formats.front(), kAudioChannelLayoutTag_AudioUnit_5_1);

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
    for (auto it = _chains.begin(); it != _chains.end(); /* in body */)
    {
      const auto &chain = *it;
      if ([chain->_identifier.outDeviceUID isEqual:identifier.outDeviceUID])
        it = _chains.erase(it);
      else
        ++it;
    }
    // also remove from desired active
    [_desiredActiveChains filterUsingPredicate:[NSPredicate predicateWithFormat:@"outDeviceUID != %@", identifier.outDeviceUID]];

    [_desiredActiveChains addObject:identifier];
    [[NSUserDefaults standardUserDefaults] setObject:[_desiredActiveChains.allObjects valueForKey:@"asDictionary"] forKey:@"ActiveChains"];
    AttemptToStartMissingChains();
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
  const auto inputDevices = GetDevicesWithMatchingInput(allDevices);
  if (inputDevices.empty())
  {
    NSMenuItem *item = [NSMenuItem new];
    item.title = @"No (suitable) input device";
    item.enabled = NO;
    [menu insertItem:item atIndex:insertionIndex];
    ++insertionIndex;
  }
  else if (isAudioInputUnavailable)
  {
    NSMenuItem *item = [NSMenuItem new];
    switch (_audioInputAuthorizationStatus)
    {
      case AIAuthorizationStatusRestricted:
        item.title = @"Access to audio input is restricted";
      break;
      case AIAuthorizationStatusDenied:
        item.title = @"Access to audio input denied";
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

  for (const auto &inDevice : inputDevices)
  {
    NSString *inDeviceName = CFBridgingRelease(CAHelper::GetStringProperty(inDevice._device, CAHelper::ObjectNameAddress));

    for (auto inStreamIdx = NSUInteger{0}; inStreamIdx < inDevice._streams.size(); ++inStreamIdx)
    {
      for (const auto &outDevice : outputDevices)
      {
        NSString *outDeviceName = CFBridgingRelease(CAHelper::GetStringProperty(outDevice._device, CAHelper::ObjectNameAddress));

        for (auto outStreamIdx = NSUInteger{0}; outStreamIdx < outDevice._streams.size(); ++outStreamIdx)
        {
          ForwardingChainIdentifier *identifier = [[ForwardingChainIdentifier alloc] initWithInDeviceUID:inDevice._uid andInStreamIndex:inStreamIdx andOutDeviceUID:outDevice._uid andOutStreamIndex:outStreamIdx];
          const AudioStreamBasicDescription &outFormat = outDevice._streams[outStreamIdx]._formats.front();
          NSMenuItem *item = [NSMenuItem new];
          if (inDevice._streams.size() > 1 || outDevice._streams.size() > 1)
            item.title = [NSString stringWithFormat:@"%@ (%lu) → %@ (%lu)", inDeviceName, inStreamIdx, outDeviceName, outStreamIdx];
          else
            item.title = [NSString stringWithFormat:@"%@ → %@", inDeviceName, outDeviceName];

          item.enabled = !inputDevices.empty() && !isAudioInputUnavailable; // either allowed or undetermined
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
    @"ActiveChains" : @[]
  }];

  { // read defaults
    _enableUpmix = [[NSUserDefaults standardUserDefaults] boolForKey:@"Upmix"];

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

- (void)applicationWillTerminate:(NSNotification *)aNotification
{
  [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:self];

  _chains.clear();

  [_statusItem.statusBar removeStatusItem:_statusItem];
}

@end
