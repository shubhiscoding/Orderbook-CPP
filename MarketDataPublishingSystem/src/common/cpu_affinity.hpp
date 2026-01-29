#pragma once

#include <sched.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include "logger.hpp"

namespace mds {

inline int get_num_cores() {
    return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
}

inline bool set_cpu_affinity(int core_id) {
    int num_cores = get_num_cores();

    if (core_id < 0 || core_id >= num_cores) {
        log_error("Invalid core_id {}. System has {} cores (0-{})",
                  core_id, num_cores, num_cores - 1);
        return false;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    int result = sched_setaffinity(0, sizeof(cpuset), &cpuset);

    if (result == 0) {
        log_info("Pinned process to CPU core {} (of {} cores)", core_id, num_cores);
        return true;
    } else {
        log_error("Failed to set CPU affinity: {}", strerror(errno));
        return false;
    }
}

inline int get_current_core() {
    return sched_getcpu();
}

inline void print_cpu_info() {
    int num_cores = get_num_cores();
    int current_core = get_current_core();

    log_info("CPU Information:");
    log_info("  - Total cores: {}", num_cores);
    log_info("  - Currently running on core: {}", current_core);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0) {
        std::string allowed_cores;
        for (int i = 0; i < num_cores; i++) {
            if (CPU_ISSET(i, &cpuset)) {
                if (!allowed_cores.empty()) allowed_cores += ", ";
                allowed_cores += std::to_string(i);
            }
        }
        log_info("  - Allowed cores: {}", allowed_cores);
    }
}

}  // namespace mds
