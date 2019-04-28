//
//  AVFoundationHelper.mm
//  SoundPusher
//
//  Created by Daniel Vollmer on 28.04.19.
//  Copyright © 2019 [maven] heavy industries. All rights reserved.
//

#import <AVFoundation/AVFoundation.h>

#import "AVFoundationHelper.h"

AIAuthorizationStatus GetAudioInputAuthorizationStatus()
{
  switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio])
  {
    case AVAuthorizationStatusRestricted:
      return AIAuthorizationStatusRestricted;
    case AVAuthorizationStatusDenied:
      return AIAuthorizationStatusDenied;
    case AVAuthorizationStatusNotDetermined:
      return AIAuthorizationStatusNotDetermined;
    case AVAuthorizationStatusAuthorized:
      return AIAuthorizationStatusAuthorized;
  }
}

AIAuthorizationStatus DetermineAudioInputAuthorization(void (^completionHandler)(AIAuthorizationStatus status))
{
  const AIAuthorizationStatus status = GetAudioInputAuthorizationStatus();
  if (status == AIAuthorizationStatusNotDetermined)
  {
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted)
    {
      const AIAuthorizationStatus status = granted ? AIAuthorizationStatusAuthorized : AIAuthorizationStatusDenied;
      completionHandler(status);
    }];
  }
  return status;
}
