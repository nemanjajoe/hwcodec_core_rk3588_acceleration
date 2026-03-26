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

  // Optional rc tuning for rkmpp.
  int qp_min = 10;
  int qp_max = 48;

  // JPEG decode path tuning.
  bool prefer_mpp_jpeg_decoder = true;
  // MPP timeout semantics: 0=non-block, <0=block, >0=timeout(ms).
  int jpeg_mpp_output_timeout_ms = 0;
  int jpeg_mpp_put_retry = 5;
  int jpeg_mpp_get_retry = 20;
  int jpeg_mpp_put_retry_sleep_ms = 1;
  int jpeg_mpp_get_retry_sleep_ms = 1;
  bool jpeg_mpp_set_packet_eos = false;

  bool debug = false;
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

  // Asynchronous pipeline API for higher throughput:
  // submit multiple inputs first, then receive packets in FIFO order.
  bool submit_bgr(const uint8_t* bgr_data,
                  int width,
                  int height,
                  int stride_bytes);
  bool submit_jpeg(const uint8_t* jpeg_data, size_t jpeg_size);
  bool receive_packet(EncodedPacket& out_packet);
  bool flush(EncodedPacket& out_packet);

 private:
  class Impl;
  Impl* impl_;
};

}  // namespace hwcodec_core
