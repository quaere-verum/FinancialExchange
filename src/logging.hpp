#pragma once

#include <ostream>

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

constexpr const char* LOG_LEVEL_NAMES[] = {
    "DEBUG",
    "INFO",
    "WARNING",
    "ERROR",
    "FATAL"
};

template<typename C, typename T>
std::basic_ostream<C, T>& operator<<(std::basic_ostream<C, T>& strm, LogLevel lvl) {
    auto levelNumber = static_cast<int>(lvl);
    strm << LOG_LEVEL_NAMES[levelNumber];
    return strm;
}

#define TG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(loggerName, channelName)\
    BOOST_LOG_INLINE_GLOBAL_LOGGER_CTOR_ARGS(loggerName,\
        boost::log::sources::severity_channel_logger<LogLevel>,\
        (boost::log::keywords::channel = (channelName)));

#define RLOG(loggerName, logLevel) BOOST_LOG_SEV(loggerName::get(), (logLevel))
