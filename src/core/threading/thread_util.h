#pragma once

#include <pthread.h>
#include <string>
#include <thread>

namespace illuminator {

// Linux thread name limit: 15 chars + null terminator (TASK_COMM_LEN = 16)
inline void SetThreadName(const std::string& name) {
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}

inline void SetThreadName(std::thread& t, const std::string& name) {
    if (t.joinable()) {
        pthread_setname_np(t.native_handle(), name.substr(0, 15).c_str());
    }
}

}  // namespace illuminator
