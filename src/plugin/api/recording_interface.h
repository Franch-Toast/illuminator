#pragma once

#include <cstdint>
#include <string>

namespace illuminator {

struct RecordingSession {
    std::string file_path;
    std::string feature_name;
    uint64_t bytes_written = 0;
    uint64_t batches_written = 0;
};

class RecordableInterface {
public:
    virtual ~RecordableInterface() = default;
    virtual void StartRecording() = 0;
    virtual void StopRecording() = 0;
    virtual bool IsRecording() const = 0;
    virtual RecordingSession GetSession() const = 0;
};

}  // namespace illuminator
