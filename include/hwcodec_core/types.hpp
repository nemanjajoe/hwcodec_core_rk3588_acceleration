#pragma once

#include <cstdint>
#include <vector>

namespace hwcodec_core {

struct EncodedPacket {
  uint64_t stamp_ns = 0;
  bool is_keyframe = false;
  std::vector<uint8_t> payload;
};

}  // namespace hwcodec_core
