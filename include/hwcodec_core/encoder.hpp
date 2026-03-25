#pragma once

#include "hwcodec_core/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hwcodec_core {

struct EncoderConfig {
  std::string codec_name = "hevc_rkmpp";
  int width = 1920;
  int height = 1080;
  int fps = 25;
  int bitrate = 4000000;
  int gop = 25;
  int bf = 0;
  std::string rc_mode = "cbr";
  std::string profile = "main";
};

class Encoder {
 public:
  Encoder();
  ~Encoder();

  bool init(const EncoderConfig& config);

  bool encode_bgr(const uint8_t* bgr_data,
                  int width,
                  int height,
                  int stride_bytes,
                  EncodedPacket& out_packet);

  bool encode_jpeg(const uint8_t* jpeg_data,
                   size_t jpeg_size,
                   EncodedPacket& out_packet);

 private:
  class Impl;
  Impl* impl_;
};

}  // namespace hwcodec_core
