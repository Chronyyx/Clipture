#pragma once
#include <cstdint>
namespace clipture::replay {
struct InPlaceIo {
    uint64_t mediaWritten = 0, metadataWritten = 0, bytesRead = 0;
};
}
