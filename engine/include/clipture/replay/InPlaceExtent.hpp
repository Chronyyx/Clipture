#pragma once
#include "clipture/replay/PayloadExtent.hpp"
namespace clipture::platform::windows { class InPlaceFile; }
namespace clipture::replay {
// A lease on one immutable range, not on every byte in a reusable arena.
class InPlaceExtent : public PayloadExtentSource {
public:
    virtual std::shared_ptr<platform::windows::InPlaceFile> file() const = 0;
};
}
