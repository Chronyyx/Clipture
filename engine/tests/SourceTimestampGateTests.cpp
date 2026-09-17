#include "clipture/SourceTimestampGate.hpp"
#include <cstdlib>
#include <iostream>
static void require(bool value) { if (!value) std::abort(); }
int main() {
    clipture::SourceTimestampGate gate;
    require(!gate.accept(0) && !gate.accept(-1));
    require(gate.accept(100));
    require(!gate.accept(100) && !gate.accept(99));
    require(gate.last() == 100);
    // A late callback must not advance to wall time and poison later sources.
    require(gate.accept(101));
    for (int i = 0; i < 1000; ++i) require(!gate.accept(100));
    require(gate.accept(102));
    // Recording cadence can repeat the same accepted source independently.
    require(!gate.accept(102));
    gate.reset();
    require(gate.accept(1));
    std::cout << "Source timestamp gate rejects stale frames without retimestamping or advancing its watermark.\n";
}
