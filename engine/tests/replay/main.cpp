#include "TestSupport.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--fixture") {
            replay_tests::runFixtureParity(argv[2], argv[3]);
        } else if (argc == 3 && std::string_view(argv[1]) == "--clone-probe") {
            replay_tests::testWindowsClone(argv[2]);
        } else if (argc == 1) {
            replay_tests::testPacker();
            replay_tests::testStoreAndMux();
            replay_tests::testPayloadLayout();
            replay_tests::testPayloadCopy();
            replay_tests::testExtentStore();
            replay_tests::testAlignedLayout();
            replay_tests::testClonePolicy();
            replay_tests::testCloneJob();
            replay_tests::testInPlace();
            replay_tests::testInPlaceArchive();
            replay_tests::testWindowsClone();
            std::cout << "Payload layout, bounded copy and overlapping extent pins passed.\n";
            std::cout << "MP4-ready packing, fallback, snapshot lifetime and mux parity passed.\n";
        } else {
            throw std::runtime_error("Usage: clipture_replay_tests [--fixture input.h264 output-directory | --clone-probe existing-scratch-parent]");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Replay test failed: " << error.what() << '\n';
        return 1;
    }
}
