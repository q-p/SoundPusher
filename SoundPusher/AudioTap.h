//
//  AudioTap.h
//  SoundPusher
//
//  Created by Daniel Vollmer on 18/04/2025.
//
//

#import <Foundation/NSString.h>
#import <CoreAudio/AudioHardwareBase.h>

/// RAII class wrapping a CATap.
struct AudioTap
{
  AudioTap(NSString *deviceUID, const NSInteger streamIndex);
  AudioTap(AudioTap &&);
  ~AudioTap();

  AudioObjectID _tap;
  NSString *_deviceUID;
  NSString *_tapUID;
};

#define kAggregateDeviceUIDPrefix @"de.maven.SoundPusher.Aggregate."

/// RAII class wrapping an aggregate device containing a "real" device and a tap for it.
struct AggregateTappedDevice
{
  AggregateTappedDevice(AudioTap &&audioTap, bool enableDriftCompensation);
  ~AggregateTappedDevice();
  AggregateTappedDevice(AggregateTappedDevice &&) = delete; // non-copyable, non-movable

  AudioTap _audioTap;
  AudioObjectID _aggregateDevice;
};
