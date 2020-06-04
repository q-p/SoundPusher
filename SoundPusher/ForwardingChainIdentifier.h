//
//  ForwardingChainIdentifier.h
//  SoundPusher
//
//  Created by Daniel Vollmer on 20/12/2015.
//
//

#import <Foundation/Foundation.h>

@interface ForwardingChainIdentifier : NSObject
- (nullable instancetype)init NS_UNAVAILABLE;
- (nonnull instancetype)initWithInDeviceUID:(nonnull NSString *)theInDeviceUID andInStreamIndex:(NSUInteger)theInStreamIndex andOutDeviceUID:(nonnull NSString *)theOutDeviceUID andOutStreamIndex:(NSUInteger)theOutStreamIndex NS_DESIGNATED_INITIALIZER;
+ (nullable instancetype)identifierWithDictionary:(nullable NSDictionary *)dict;
@property (readonly, nonatomic, nonnull) NSString *inDeviceUID;
@property (readonly, nonatomic) NSUInteger inStreamIndex;
@property (readonly, nonatomic, nonnull) NSString *outDeviceUID;
@property (readonly, nonatomic) NSUInteger outStreamIndex;
@property (readonly, nonatomic, nonnull) NSDictionary *asDictionary;
// format and inDevice are implicit
@end
