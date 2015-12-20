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

- (instancetype)initWithOutDeviceUID:(NSString *)theOutDeviceUID andOutStreamIndex:(NSUInteger)theOutStreamIndex
{
  if (self = [super init])
  {
    _outDeviceUID = [theOutDeviceUID copy];
    _outStreamIndex = theOutStreamIndex;
  }
  return self;
}

+ (instancetype)identifierWithDictionary:(NSDictionary *)dict
{
  NSString *outDeviceUID = dict[@"Device"];
  NSNumber *outStreamIndex = dict[@"Stream"];
  if (outDeviceUID && outStreamIndex && [outStreamIndex isKindOfClass:NSNumber.class])
    return [[ForwardingChainIdentifier alloc] initWithOutDeviceUID:outDeviceUID andOutStreamIndex:outStreamIndex.unsignedIntegerValue];
  return nil;
}

- (NSString *)description
{
  return [NSString stringWithFormat:@"<%@: %p; outDeviceUID = \"%@\"; outStreamIndex = %zu>", [self class], self,
    _outDeviceUID, _outStreamIndex];
}

- (NSDictionary *)asDictionary
{
  return @{@"Device": _outDeviceUID, @"Stream" : [NSNumber numberWithUnsignedInteger:_outStreamIndex]};
}

- (BOOL)isEqual: (id)someOther
{
  if (![someOther isKindOfClass:ForwardingChainIdentifier.class])
    return NO;
  ForwardingChainIdentifier *other = someOther;
  return _outStreamIndex == other->_outStreamIndex && [_outDeviceUID isEqual: other->_outDeviceUID];
}

#define NSUINT_BIT (CHAR_BIT * sizeof(NSUInteger))
#define NSUINTROTATE(val, howmuch) ((((NSUInteger)val) << howmuch) | (((NSUInteger)val) >> (NSUINT_BIT - howmuch)))
- (NSUInteger)hash
{
  NSUInteger hash = _outDeviceUID.hash;
  return NSUINTROTATE(hash, NSUINT_BIT / 2) ^ _outStreamIndex;
}

@end
