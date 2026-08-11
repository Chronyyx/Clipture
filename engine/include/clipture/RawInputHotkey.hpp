#pragma once

#include <functional>
#include <memory>
#include <string>

namespace clipture {

class RawInputHotkey {
public:
    using TriggerCallback = std::function<void()>;

    explicit RawInputHotkey(TriggerCallback onTrigger);
    ~RawInputHotkey();

    RawInputHotkey(const RawInputHotkey&) = delete;
    RawInputHotkey& operator=(const RawInputHotkey&) = delete;

    bool configure(const std::string& accelerator);
    bool ready() const;
    std::string status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipture
