#pragma once

#include "hwcodec_core/types.hpp"

#include <opencv2/opencv.hpp>

namespace hwcodec_core {

struct DecoderConfig {
  int width = 1920;
  int height = 1080;
  int jpeg_quality = 80;
  int mpp_split_mode = 1;
  // MPP timeout semantics: 0=non-block, <0=block, >0=timeout(ms).
  int mpp_output_timeout_ms = 0;
  int mpp_put_retry = 5;
  int mpp_get_retry = 15;
  int mpp_put_retry_sleep_ms = 2;
  int mpp_get_retry_sleep_ms = 1;
  bool debug = false;
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
