//
//  AudioTap.mm
//  SoundPusher
//
//  Created by Daniel Vollmer on 18/04/2025.
//
//

#import <Foundation/NSDictionary.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>

#include "AudioTap.h"

#include "CoreAudioHelper.hpp"

static const AudioObjectPropertyAddress TapUIDAddress = {kAudioTapPropertyUID, 0, 0};

AudioTap::AudioTap(NSString *deviceUID, const NSInteger streamIndex)
: _tap(kAudioObjectUnknown)
, _deviceUID([deviceUID copy])
{
  CATapDescription *tapConfig = [[CATapDescription alloc] initExcludingProcesses:@[] andDeviceUID:_deviceUID withStream:streamIndex];

  tapConfig.privateTap = YES;
  tapConfig.muteBehavior = CATapMutedWhenTapped;
  tapConfig.name = [NSString stringWithFormat:@"SoundPusher Tap for '%@'", deviceUID];

  OSStatus status = AudioHardwareCreateProcessTap(tapConfig, &_tap);
  if (status != noErr)
    throw CAHelper::CoreAudioException("AudioHardwareCreateProcessTap()", status);
  _tapUID = CFBridgingRelease(CAHelper::GetStringProperty(_tap, TapUIDAddress));
}

AudioTap::AudioTap(AudioTap &&other)
: _tap(other._tap)
, _deviceUID(other._deviceUID)
, _tapUID(other._tapUID)
{
  other._tap = kAudioObjectUnknown;
}

AudioTap::~AudioTap()
{
  if (_tap != kAudioObjectUnknown)
    AudioHardwareDestroyProcessTap(_tap);
}

AggregateTappedDevice::AggregateTappedDevice(AudioTap &&audioTap, const bool enableDriftCompensation)
: _audioTap(std::move(audioTap))
{
  NSDictionary *dict = @{
    @kAudioAggregateDeviceUIDKey : [NSString stringWithFormat:@"%@%@", kAggregateDeviceUIDPrefix, _audioTap._deviceUID],
    @kAudioAggregateDeviceNameKey : [NSString stringWithFormat:@"SoundPusher Aggregate for '%@'", _audioTap._deviceUID],
    // it seems we only need the tap, not the actual device in there
//    @kAudioAggregateDeviceMainSubDeviceKey : _audioTap._deviceUID,
//    @kAudioAggregateDeviceSubDeviceListKey : @[
//      @{
//        @kAudioSubDeviceUIDKey : _audioTap._deviceUID,
//      },
//    ],
    @kAudioAggregateDeviceIsPrivateKey : @YES,
    @kAudioAggregateDeviceTapListKey : @[
      @{
        @kAudioSubTapUIDKey : _audioTap._tapUID,
        // since we only have a single device, drift compensation seems pointless?
        @kAudioSubTapDriftCompensationKey : [NSNumber numberWithBool:enableDriftCompensation],
      },
    ],
//    @kAudioAggregateDeviceTapAutoStartKey : @YES,
  };

  OSStatus status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)dict, &_aggregateDevice);
  if (status != noErr)
    throw CAHelper::CoreAudioException("AudioHardwareCreateAggregateDevice()", status);
}

AggregateTappedDevice::~AggregateTappedDevice()
{
  AudioHardwareDestroyAggregateDevice(_aggregateDevice);
}
