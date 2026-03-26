#include "hwcodec_core/decoder.hpp"

#include "hwcodec_core/internal/rga_converter.hpp"
#include "hwcodec_core/internal/rkmpp_decoder.hpp"

#include <opencv2/opencv.hpp>

#include <cstring>
#include <iostream>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace hwcodec_core {

class Decoder::Impl {
 public:
  ~Impl() { destroy_soft_decoder(); }

  bool init(const DecoderConfig& config) {
    cfg_ = config;
    debug_ = cfg_.debug;

    if (cfg_.width <= 0 || cfg_.height <= 0 || cfg_.jpeg_quality < 1 || cfg_.jpeg_quality > 100 ||
        cfg_.mpp_put_retry <= 0 || cfg_.mpp_get_retry <= 0 ||
        cfg_.mpp_put_retry_sleep_ms < 0 || cfg_.mpp_get_retry_sleep_ms < 0) {
      std::cerr << "[Decoder] invalid config" << std::endl;
      return false;
    }
    if (debug_) {
      std::cerr << "[Decoder][debug] init width=" << cfg_.width
                << " height=" << cfg_.height
                << " jpeg_quality=" << cfg_.jpeg_quality
                << " split_mode=" << cfg_.mpp_split_mode
                << " timeout_ms=" << cfg_.mpp_output_timeout_ms
                << " put_retry=" << cfg_.mpp_put_retry
                << " get_retry=" << cfg_.mpp_get_retry
                << " put_sleep_ms=" << cfg_.mpp_put_retry_sleep_ms
                << " get_sleep_ms=" << cfg_.mpp_get_retry_sleep_ms << std::endl;
    }

    if (decoder_.init(cfg_.width,
                      cfg_.height,
                      cfg_.mpp_split_mode,
                      cfg_.mpp_output_timeout_ms,
                      cfg_.mpp_put_retry,
                      cfg_.mpp_get_retry,
                      cfg_.mpp_put_retry_sleep_ms,
                      cfg_.mpp_get_retry_sleep_ms,
                      cfg_.debug) != 0) {
      std::cerr << "[Decoder] MPP init failed" << std::endl;
      return false;
    }
    if (rga_.init() != 0) {
      std::cerr << "[Decoder] RGA init failed" << std::endl;
      return false;
    }
    if (!init_soft_decoder()) {
      std::cerr << "[Decoder] soft HEVC decoder init failed" << std::endl;
      return false;
    }

    initialized_ = true;
    return true;
  }

  bool decode_to_bgr(const EncodedPacket& packet, cv::Mat& bgr_out) {
    if (!initialized_ || packet.payload.empty()) {
      if (debug_) {
        std::cerr << "[Decoder][debug] decode_to_bgr invalid state or empty payload" << std::endl;
      }
      return false;
    }

    const int dma_fd = decoder_.decode(packet.payload.data(), packet.payload.size());
    if (dma_fd < 0) {
      if (debug_) {
        std::cerr << "[Decoder][debug] MPP decode returned invalid dma_fd, fallback to software decode"
                  << std::endl;
      }
      return decode_to_bgr_soft(packet, bgr_out);
    }

    const int stride_w = decoder_.get_hor_stride();
    const int stride_h = decoder_.get_ver_stride();
    if (stride_w <= 0 || stride_h <= 0) {
      if (debug_) {
        std::cerr << "[Decoder][debug] invalid stride from decoder: "
                  << stride_w << "x" << stride_h
                  << ", fallback to software decode" << std::endl;
      }
      return decode_to_bgr_soft(packet, bgr_out);
    }

    if (rga_.nv12_dma_to_bgr(dma_fd, stride_w, stride_h, cfg_.width, cfg_.height, bgr_out) != 0) {
      if (debug_) {
        std::cerr << "[Decoder][debug] RGA nv12_dma_to_bgr failed, fallback to software decode" << std::endl;
      }
      return decode_to_bgr_soft(packet, bgr_out);
    }
    if (debug_) {
      std::cerr << "[Decoder][debug] decode_to_bgr success, output=" << bgr_out.cols
                << "x" << bgr_out.rows << std::endl;
    }
    return !bgr_out.empty();
  }

  bool decode_to_jpeg(const EncodedPacket& packet, std::vector<uint8_t>& jpeg_out) {
    cv::Mat bgr;
    if (!decode_to_bgr(packet, bgr)) {
      if (debug_) {
        std::cerr << "[Decoder][debug] decode_to_bgr failed before jpeg encode" << std::endl;
      }
      return false;
    }
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, cfg_.jpeg_quality};
    const bool ok = cv::imencode(".jpg", bgr, jpeg_out, params);
    if (debug_) {
      std::cerr << "[Decoder][debug] imencode result=" << (ok ? "ok" : "fail")
                << " jpeg_bytes=" << jpeg_out.size() << std::endl;
    }
    return ok;
  }

 private:
  bool init_soft_decoder() {
    destroy_soft_decoder();

    const AVCodec* codec = avcodec_find_decoder_by_name("hevc");
    if (!codec) {
      codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    }
    if (!codec) {
      return false;
    }

    sw_ctx_ = avcodec_alloc_context3(codec);
    if (!sw_ctx_) {
      return false;
    }
    if (avcodec_open2(sw_ctx_, codec, nullptr) < 0) {
      destroy_soft_decoder();
      return false;
    }

    sw_frame_ = av_frame_alloc();
    sw_packet_ = av_packet_alloc();
    if (!sw_frame_ || !sw_packet_) {
      destroy_soft_decoder();
      return false;
    }

    sw_ready_ = true;
    return true;
  }

  void destroy_soft_decoder() {
    if (sws_ctx_) {
      sws_freeContext(sws_ctx_);
      sws_ctx_ = nullptr;
    }
    if (sw_packet_) {
      av_packet_free(&sw_packet_);
      sw_packet_ = nullptr;
    }
    if (sw_frame_) {
      av_frame_free(&sw_frame_);
      sw_frame_ = nullptr;
    }
    if (sw_ctx_) {
      avcodec_free_context(&sw_ctx_);
      sw_ctx_ = nullptr;
    }
    sw_ready_ = false;
  }

  bool decode_to_bgr_soft(const EncodedPacket& packet, cv::Mat& bgr_out) {
    if (!sw_ready_ || !sw_ctx_ || !sw_packet_ || !sw_frame_) {
      return false;
    }

    av_packet_unref(sw_packet_);
    int ret = av_new_packet(sw_packet_, static_cast<int>(packet.payload.size()));
    if (ret < 0) {
      if (debug_) {
        std::cerr << "[Decoder][debug] av_new_packet failed ret=" << ret << std::endl;
      }
      return false;
    }
    std::memcpy(sw_packet_->data, packet.payload.data(), packet.payload.size());

    ret = avcodec_send_packet(sw_ctx_, sw_packet_);
    if (ret < 0) {
      if (debug_) {
        std::cerr << "[Decoder][debug] avcodec_send_packet failed ret=" << ret << std::endl;
      }
      return false;
    }

    while (true) {
      ret = avcodec_receive_frame(sw_ctx_, sw_frame_);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        if (debug_) {
          std::cerr << "[Decoder][debug] software decoder no frame ret=" << ret << std::endl;
        }
        return false;
      }
      if (ret < 0) {
        if (debug_) {
          std::cerr << "[Decoder][debug] avcodec_receive_frame failed ret=" << ret << std::endl;
        }
        return false;
      }
      break;
    }

    const int out_w = sw_frame_->width;
    const int out_h = sw_frame_->height;
    if (out_w <= 0 || out_h <= 0) {
      return false;
    }

    bgr_out.create(out_h, out_w, CV_8UC3);
    uint8_t* dst_data[4] = {bgr_out.data, nullptr, nullptr, nullptr};
    int dst_linesize[4] = {static_cast<int>(bgr_out.step), 0, 0, 0};

    sws_ctx_ = sws_getCachedContext(sws_ctx_,
                                    out_w, out_h, static_cast<AVPixelFormat>(sw_frame_->format),
                                    out_w, out_h, AV_PIX_FMT_BGR24,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) {
      if (debug_) {
        std::cerr << "[Decoder][debug] sws_getCachedContext failed" << std::endl;
      }
      return false;
    }

    const int scaled = sws_scale(sws_ctx_,
                                 sw_frame_->data,
                                 sw_frame_->linesize,
                                 0,
                                 out_h,
                                 dst_data,
                                 dst_linesize);
    if (scaled <= 0) {
      if (debug_) {
        std::cerr << "[Decoder][debug] sws_scale failed scaled=" << scaled << std::endl;
      }
      return false;
    }

    if (debug_) {
      std::cerr << "[Decoder][debug] software decode success output=" << bgr_out.cols
                << "x" << bgr_out.rows << std::endl;
    }
    return !bgr_out.empty();
  }

 private:
  DecoderConfig cfg_;
  MppDecoder decoder_;
  RgaConverter rga_;
  bool initialized_ = false;
  bool debug_ = false;

  AVCodecContext* sw_ctx_ = nullptr;
  AVFrame* sw_frame_ = nullptr;
  AVPacket* sw_packet_ = nullptr;
  SwsContext* sws_ctx_ = nullptr;
  bool sw_ready_ = false;
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
