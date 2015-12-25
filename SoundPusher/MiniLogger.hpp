//
//  MiniLogger.hpp
//  SoundPusher
//
//  Created by Daniel Vollmer on 23/12/2015.
//  Copyright © 2015 [maven] heavy industries. All rights reserved.
//

#ifndef MiniLogger_hpp
#define MiniLogger_hpp

#include <asl.h>

struct MiniLogger
{
  enum Level
  {
    LogEmergency = ASL_LEVEL_EMERG,
    LogAlert     = ASL_LEVEL_ALERT,
    LogCrit      = ASL_LEVEL_CRIT,
    LogErr       = ASL_LEVEL_ERR,
    LogWarning   = ASL_LEVEL_WARNING,
    LogNotice    = ASL_LEVEL_NOTICE,
    LogInfo      = ASL_LEVEL_INFO,
    LogDebug     = ASL_LEVEL_DEBUG
  };

  MiniLogger(const Level level = LogNotice, const char *subsystem = nullptr);
  ~MiniLogger();

  void SetLevel(const Level level) { _level = level; }
  Level GetLevel() const { return _level; }

  void Emergency(const char *format, ...) const;
  void Alert(const char *format, ...) const;
  void Crit(const char *format, ...) const;
  void Err(const char *format, ...) const;
  void Warning(const char *format, ...) const;
  void Notice(const char *format, ...) const;
  void Info(const char *format, ...) const;
  void Debug(const char *format, ...) const;

protected:
  Level _level;
  asl_object_t _clientHandle;
  asl_object_t _messageDict;
};

extern MiniLogger DefaultLogger;

#endif /* MiniLogger_hpp */
