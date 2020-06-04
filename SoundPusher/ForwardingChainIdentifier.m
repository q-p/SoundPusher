//
//  ForwardingChainIdentifier.m
//  SoundPusher
//
//  Created by Daniel Vollmer on 20/12/2015.
//
//

#import "ForwardingChainIdentifier.h"

@implementation ForwardingChainIdentifier

- (instancetype)init { @throw nil; }

- (instancetype)initWithInDeviceUID:(NSString *)theInDeviceUID andInStreamIndex:(NSUInteger)theInStreamIndex andOutDeviceUID:(NSString *)theOutDeviceUID andOutStreamIndex:(NSUInteger)theOutStreamIndex
{
  if (self = [super init])
  {
    _inDeviceUID = [theInDeviceUID copy];
    _inStreamIndex = theInStreamIndex;
    _outDeviceUID = [theOutDeviceUID copy];
    _outStreamIndex = theOutStreamIndex;
  }
  return self;
}

+ (instancetype)identifierWithDictionary:(NSDictionary *)dict
{
  NSString *inDeviceUID = dict[@"InDevice"];
  NSNumber *inStreamIndex = dict[@"InStream"];
  NSString *outDeviceUID = dict[@"OutDevice"];
  NSNumber *outStreamIndex = dict[@"OutStream"];
  if (inDeviceUID && inStreamIndex && [inStreamIndex isKindOfClass:NSNumber.class]
    && outDeviceUID && outStreamIndex && [outStreamIndex isKindOfClass:NSNumber.class])
    return [[ForwardingChainIdentifier alloc]
      initWithInDeviceUID:inDeviceUID andInStreamIndex:inStreamIndex.unsignedIntegerValue
      andOutDeviceUID:outDeviceUID andOutStreamIndex:outStreamIndex.unsignedIntegerValue];
  return nil;
}

- (NSString *)description
{
  return [NSString stringWithFormat:@"<%@: %p; inDeviceUID = \"%@\"; inStreamIndex = %zu; outDeviceUID = \"%@\"; outStreamIndex = %zu>", [self class], (void *)self, _inDeviceUID, _inStreamIndex, _outDeviceUID, _outStreamIndex];
}

- (NSDictionary *)asDictionary
{
  return @{@"InDevice": _inDeviceUID, @"InStream" : [NSNumber numberWithUnsignedInteger:_inStreamIndex], @"OutDevice": _outDeviceUID, @"OutStream" : [NSNumber numberWithUnsignedInteger:_outStreamIndex]};
}

- (BOOL)isEqual: (id)someOther
{
  if (![someOther isKindOfClass:ForwardingChainIdentifier.class])
    return NO;
  ForwardingChainIdentifier *other = someOther;
  return _outStreamIndex == other->_outStreamIndex && _inStreamIndex == other->_inStreamIndex
    && [_inDeviceUID isEqual: other->_inDeviceUID] && [_outDeviceUID isEqual: other->_outDeviceUID];
}

#define NSUINT_BIT (CHAR_BIT * sizeof(NSUInteger))
#define NSUINTROTATE(val, howmuch) ((((NSUInteger)val) << howmuch) | (((NSUInteger)val) >> (NSUINT_BIT - howmuch)))
- (NSUInteger)hash
{
  NSUInteger hash = _inDeviceUID.hash;
  hash = NSUINTROTATE(hash, NSUINT_BIT / 2) ^ _inStreamIndex;
  hash = NSUINTROTATE(hash, NSUINT_BIT / 4) ^ _outDeviceUID.hash;
  hash = NSUINTROTATE(hash, 7) ^ _outStreamIndex;
  return hash;
}

@end
