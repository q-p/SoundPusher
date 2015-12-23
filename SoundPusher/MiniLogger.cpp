//
//  MiniLogger.cpp
//  SoundPusher
//
//  Created by Daniel Vollmer on 23/12/2015.
//  Copyright © 2015 [maven] heavy industries. All rights reserved.
//

#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <syslog.h>

#include "MiniLogger.hpp"


MiniLogger DefaultLogger;


static void DoLog(int priority, const char *format, va_list args)
{
  va_list argCopy;
  va_copy(argCopy, args);
  vsyslog(priority, format, args);
  const auto now = std::time(nullptr);
  const auto local = std::localtime(&now);
  fprintf(stderr, "%04i-%02i-%02i %02i:%02i:%02i ", 1900 + local->tm_year, local->tm_mon, local->tm_mday, local->tm_hour, local->tm_min, local->tm_sec);
  vfprintf(stderr, format, argCopy);
  va_end(argCopy);
  fputs("\n", stderr);
}

MiniLogger::MiniLogger(const Level level)
: _level(level)
{ }

#define FORWARD_METHOD_DEF(Name, Prio) \
void MiniLogger::Name(const char *format, ...) const \
{ \
  if (_level < Log##Name) \
    return; \
  va_list args; \
  va_start(args, format); \
  DoLog(Prio, format, args); \
  va_end(args); \
}

FORWARD_METHOD_DEF(Error, LOG_ERR)
FORWARD_METHOD_DEF(Warn, LOG_WARNING)
FORWARD_METHOD_DEF(Note, LOG_NOTICE)
FORWARD_METHOD_DEF(Info, LOG_INFO)
FORWARD_METHOD_DEF(Debug, LOG_DEBUG)
