//
//  AVFoundationHelper.h
//  SoundPusher
//
//  Created by Daniel Vollmer on 28.04.19.
//  Copyright © 2019 [maven] heavy industries. All rights reserved.
//

#ifndef AVFoundationHelper_h
#define AVFoundationHelper_h

#import <Foundation/Foundation.h>

/// Renamed AVAuthorizationStatus due to conflicting type of same name in FFmpeg.
typedef NS_ENUM(NSUInteger, AIAuthorizationStatus)
{
  AIAuthorizationStatusNotDetermined = 0,
  AIAuthorizationStatusRestricted    = 1,
  AIAuthorizationStatusDenied        = 2,
  AIAuthorizationStatusAuthorized    = 3,
};

AIAuthorizationStatus GetAudioInputAuthorizationStatus(void);

AIAuthorizationStatus DetermineAudioInputAuthorization(void (^completionHandler)(AIAuthorizationStatus status));

#endif /* AVFoundationHelper_h */
