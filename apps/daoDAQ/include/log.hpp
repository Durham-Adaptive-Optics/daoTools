/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-05-08 10:11:42
 * @ Description: Logging Helper Macros.
 */

#pragma once

#include <fmt/format.h>
#include <daoLog.hpp>

/* Helper macros to format a log message using libfmt and return a char*
 * rather than a std::string - a limitation of dao logging API.
*/

#define LOGFMT(fmtstr, ...) (fmt::format(fmtstr, __VA_ARGS__).c_str())
