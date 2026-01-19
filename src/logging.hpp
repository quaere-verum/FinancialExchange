#pragma once

#ifndef TG_COMPILED_LOG_LEVEL
    #ifdef NDEBUG
        #define TG_COMPILED_LOG_LEVEL LogLevel::LL_WARNING
    #else
        #define TG_COMPILED_LOG_LEVEL LogLevel::LL_DEBUG
    #endif
#endif

#include <ostream>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <boost/log/keywords/channel.hpp>
#include <boost/log/sources/global_logger_storage.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_channel_logger.hpp>

enum class LogLevel : unsigned char {
    LL_DEBUG,
    LL_INFO,
    LL_WARNING,
    LL_ERROR,
    LL_FATAL
};

constexpr bool log_level_enabled(LogLevel lvl) {
    return static_cast<unsigned char>(lvl) >= static_cast<unsigned char>(TG_COMPILED_LOG_LEVEL);
}


constexpr const char* LOG_LEVEL_NAMES[] = {
    "DEBUG",
    "INFO",
    "WARNING",
    "ERROR",
    "FATAL"
};

template<typename C, typename T>
std::basic_ostream<C, T>& operator<<(std::basic_ostream<C, T>& stream, LogLevel lvl) {
    auto levelNumber = static_cast<int>(lvl);
    stream << LOG_LEVEL_NAMES[levelNumber];
    return stream;
}

#define TG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(loggerName, channelName)\
    BOOST_LOG_INLINE_GLOBAL_LOGGER_CTOR_ARGS(loggerName,\
        boost::log::sources::severity_channel_logger<LogLevel>,\
        (boost::log::keywords::channel = (channelName)));

#define RLOG(loggerName, logLevel) if constexpr (log_level_enabled(logLevel)) BOOST_LOG_SEV(loggerName::get(), (logLevel))
