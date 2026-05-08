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

#define LOGFMT(fmtstr, ...) (fmt::format(fmtstr, __VA_ARGS__).c_str())
