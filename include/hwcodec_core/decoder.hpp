#pragma once

#include "hwcodec_core/types.hpp"

#include <opencv2/opencv.hpp>

namespace hwcodec_core {

struct DecoderConfig {
  int width = 1920;
  int height = 1080;
  int jpeg_quality = 80;
};

class Decoder {
 public:
  Decoder();
  ~Decoder();

  bool init(const DecoderConfig& config);
  bool decode_to_bgr(const EncodedPacket& packet, cv::Mat& bgr_out);
  bool decode_to_jpeg(const EncodedPacket& packet, std::vector<uint8_t>& jpeg_out);

 private:
  class Impl;
  Impl* impl_;
};

}  // namespace hwcodec_core
