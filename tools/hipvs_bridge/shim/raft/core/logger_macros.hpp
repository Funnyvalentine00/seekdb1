/* Minimal raft logger stub for HIP builds. The ROCm-DS raft fork ships a real
 * implementation; upstream tarballs reference this header without providing it.
 * We only need the level constants and no-op log macros — the bridge never logs
 * through raft. */
#pragma once

#define RAPIDS_LOGGER_LOG_LEVEL_TRACE 0
#define RAPIDS_LOGGER_LOG_LEVEL_DEBUG 1
#define RAPIDS_LOGGER_LOG_LEVEL_INFO 2
#define RAPIDS_LOGGER_LOG_LEVEL_WARN 3
#define RAPIDS_LOGGER_LOG_LEVEL_ERROR 4
#define RAPIDS_LOGGER_LOG_LEVEL_CRITICAL 5
#define RAPIDS_LOGGER_LOG_LEVEL_OFF 6

#ifndef RAFT_LOG_ACTIVE_LEVEL
#define RAFT_LOG_ACTIVE_LEVEL RAPIDS_LOGGER_LOG_LEVEL_INFO
#endif

#define RAFT_LOG_TRACE(...) void(0)
#define RAFT_LOG_DEBUG(...) void(0)
#define RAFT_LOG_INFO(...) void(0)
#define RAFT_LOG_WARN(...) void(0)
#define RAFT_LOG_ERROR(...) void(0)
#define RAFT_LOG_CRITICAL(...) void(0)
