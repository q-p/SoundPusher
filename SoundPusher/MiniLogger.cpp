//
//  MiniLogger.cpp
//  SoundPusher
//
//  Created by Daniel Vollmer on 23/12/2015.
//  Copyright © 2015 [maven] heavy industries. All rights reserved.
//

#include <cstdarg>
#include <cstdio>

#include "MiniLogger.hpp"

MiniLogger DefaultLogger;

MiniLogger::MiniLogger(const Level level, const char *thread)
: _level(level), _clientHandle(0), _messageDict(0)
{
  if (thread)
  { // create a (non-thread-safe) logger for this
    _clientHandle = asl_open(NULL, NULL, ASL_OPT_STDERR | ASL_OPT_NO_DELAY);
    _messageDict = asl_new(ASL_TYPE_MSG);
    asl_set(_messageDict, ASL_KEY_SENDER, thread);
  }
  else
  {
    asl_add_log_file(NULL, STDERR_FILENO);
  }
}

MiniLogger::~MiniLogger()
{
  if (_messageDict)
    asl_release(_messageDict);
  if (_clientHandle)
    asl_close(_clientHandle);
}

#define FORWARD_METHOD_DEF(Name) \
void MiniLogger::Name(const char *format, ...) const \
{ \
  if (_level < Log##Name) \
    return; \
  va_list args; \
  va_start(args, format); \
  asl_vlog(_clientHandle, _messageDict, Log##Name, format, args); \
  va_end(args); \
}

FORWARD_METHOD_DEF(Emergency)
FORWARD_METHOD_DEF(Alert)
FORWARD_METHOD_DEF(Crit)
FORWARD_METHOD_DEF(Err)
FORWARD_METHOD_DEF(Warning)
FORWARD_METHOD_DEF(Notice)
FORWARD_METHOD_DEF(Info)
FORWARD_METHOD_DEF(Debug)
