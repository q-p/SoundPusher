//
//  MiniLogger.hpp
//  SoundPusher
//
//  Created by Daniel Vollmer on 23/12/2015.
//  Copyright © 2015 [maven] heavy industries. All rights reserved.
//

#ifndef MiniLogger_hpp
#define MiniLogger_hpp

struct MiniLogger
{
  enum Level
  {
    LogNone = 0,
    LogError,
    LogWarn,
    LogNote,
    LogInfo,
    LogDebug
  };

  MiniLogger(const Level level = LogNote);

  void SetLevel(const Level level) { _level = level; }
  Level GetLevel() const { return _level; }

  void Error(const char *format, ...) const;
  void Warn(const char *format, ...) const;
  void Note(const char *format, ...) const;
  void Info(const char *format, ...) const;
  void Debug(const char *format, ...) const;

protected:
  Level _level;
};

extern MiniLogger DefaultLogger;

#endif /* MiniLogger_hpp */
