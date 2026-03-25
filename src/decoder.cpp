#include "hwcodec_core/decoder.hpp"

#include "hwcodec_core/internal/rga_converter.hpp"
#include "hwcodec_core/internal/rkmpp_decoder.hpp"

#include <opencv2/opencv.hpp>

#include <iostream>

namespace hwcodec_core {

class Decoder::Impl {
 public:
  bool init(const DecoderConfig& config) {
    cfg_ = config;

    if (decoder_.init(cfg_.width, cfg_.height) != 0) {
      std::cerr << "[Decoder] MPP init failed" << std::endl;
      return false;
    }
    if (rga_.init() != 0) {
      std::cerr << "[Decoder] RGA init failed" << std::endl;
      return false;
    }

    initialized_ = true;
    return true;
  }

  bool decode_to_bgr(const EncodedPacket& packet, cv::Mat& bgr_out) {
    if (!initialized_ || packet.payload.empty()) {
      return false;
    }

    const int dma_fd = decoder_.decode(packet.payload.data(), packet.payload.size());
    if (dma_fd < 0) {
      return false;
    }

    const int stride_w = decoder_.get_hor_stride();
    const int stride_h = decoder_.get_ver_stride();
    if (stride_w <= 0 || stride_h <= 0) {
      return false;
    }

    cv::Mat rgb;
    if (rga_.nv12_dma_to_rgb(dma_fd, stride_w, stride_h, cfg_.width, cfg_.height, rgb) != 0) {
      return false;
    }

    cv::cvtColor(rgb, bgr_out, cv::COLOR_RGB2BGR);
    return !bgr_out.empty();
  }

  bool decode_to_jpeg(const EncodedPacket& packet, std::vector<uint8_t>& jpeg_out) {
    cv::Mat bgr;
    if (!decode_to_bgr(packet, bgr)) {
      return false;
    }

    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, cfg_.jpeg_quality};
    return cv::imencode(".jpg", bgr, jpeg_out, params);
  }

 private:
  DecoderConfig cfg_;
  MppDecoder decoder_;
  RgaConverter rga_;
  bool initialized_ = false;
};

Decoder::Decoder() : impl_(new Impl()) {}

Decoder::~Decoder() {
  delete impl_;
  impl_ = nullptr;
}

bool Decoder::init(const DecoderConfig& config) {
  return impl_->init(config);
}

bool Decoder::decode_to_bgr(const EncodedPacket& packet, cv::Mat& bgr_out) {
  return impl_->decode_to_bgr(packet, bgr_out);
}

bool Decoder::decode_to_jpeg(const EncodedPacket& packet, std::vector<uint8_t>& jpeg_out) {
  return impl_->decode_to_jpeg(packet, jpeg_out);
}

}  // namespace hwcodec_core
