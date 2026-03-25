#include "hwcodec_core/encoder.hpp"

#include <opencv2/opencv.hpp>
#include <rga/RgaApi.h>
#include <rga/im2d.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace hwcodec_core {

namespace {

std::string ff_err_str(int errnum) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_make_error_string(buf, sizeof(buf), errnum);
  return std::string(buf);
}

uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

}  // namespace

class Encoder::Impl {
 public:
  bool init(const EncoderConfig& config) {
    cfg_ = config;
    if (cfg_.width <= 0 || cfg_.height <= 0 || cfg_.fps <= 0) {
      return false;
    }

    const size_t nv12_size = static_cast<size_t>(cfg_.width) * static_cast<size_t>(cfg_.height) * 3U / 2U;
    nv12_buffer_.resize(nv12_size);

    codec_ = avcodec_find_encoder_by_name(cfg_.codec_name.c_str());
    if (!codec_) {
      std::cerr << "[Encoder] encoder not found: " << cfg_.codec_name << std::endl;
      return false;
    }

    ctx_ = avcodec_alloc_context3(codec_);
    if (!ctx_) {
      return false;
    }

    ctx_->width = cfg_.width;
    ctx_->height = cfg_.height;
    ctx_->pix_fmt = AV_PIX_FMT_NV12;
    ctx_->time_base = AVRational{1, cfg_.fps};
    ctx_->framerate = AVRational{cfg_.fps, 1};
    ctx_->bit_rate = cfg_.bitrate;
    ctx_->gop_size = cfg_.gop;
    ctx_->max_b_frames = cfg_.bf;
    if (cfg_.profile == "main") {
      ctx_->profile = FF_PROFILE_HEVC_MAIN;
    }

    AVDictionary* opts = nullptr;
    if (cfg_.rc_mode == "cbr") {
      av_dict_set(&opts, "rc_mode", "1", 0);
      av_dict_set(&opts, "qp_min", "10", 0);
      av_dict_set(&opts, "qp_max", "48", 0);
    } else {
      av_dict_set(&opts, "rc_mode", "0", 0);
    }

    const int ret = avcodec_open2(ctx_, codec_, &opts);
    if (opts) {
      av_dict_free(&opts);
    }
    if (ret < 0) {
      std::cerr << "[Encoder] avcodec_open2 failed: " << ff_err_str(ret) << std::endl;
      return false;
    }

    pkt_ = av_packet_alloc();
    if (!pkt_) {
      return false;
    }

    initialized_ = true;
    pts_ = 0;
    return true;
  }

  bool encode_bgr(const uint8_t* bgr_data,
                  int width,
                  int height,
                  int stride_bytes,
                  EncodedPacket& out_packet) {
    if (!initialized_ || !bgr_data || width <= 0 || height <= 0 || stride_bytes <= 0) {
      return false;
    }

    cv::Mat src(height, width, CV_8UC3, const_cast<uint8_t*>(bgr_data), static_cast<size_t>(stride_bytes));
    cv::Mat resized;
    if (width != cfg_.width || height != cfg_.height) {
      cv::resize(src, resized, cv::Size(cfg_.width, cfg_.height));
    }
    const cv::Mat& in = resized.empty() ? src : resized;

    if (!bgr_mat_to_nv12(in)) {
      return false;
    }

    return encode_nv12(out_packet);
  }

  bool encode_jpeg(const uint8_t* jpeg_data, size_t jpeg_size, EncodedPacket& out_packet) {
    if (!initialized_ || !jpeg_data || jpeg_size == 0) {
      return false;
    }

    cv::Mat raw(1, static_cast<int>(jpeg_size), CV_8UC1, const_cast<uint8_t*>(jpeg_data));
    cv::Mat bgr = cv::imdecode(raw, cv::IMREAD_COLOR);
    if (bgr.empty()) {
      std::cerr << "[Encoder] cv::imdecode failed" << std::endl;
      return false;
    }

    if (!bgr_mat_to_nv12(bgr)) {
      return false;
    }

    return encode_nv12(out_packet);
  }

  ~Impl() {
    if (pkt_) {
      av_packet_free(&pkt_);
    }
    if (ctx_) {
      avcodec_free_context(&ctx_);
    }
  }

 private:
  bool bgr_mat_to_nv12(const cv::Mat& bgr) {
    const size_t elem_size = bgr.elemSize();
    if (elem_size == 0 || bgr.step < static_cast<size_t>(bgr.cols) * elem_size ||
        (bgr.step % elem_size) != 0) {
      std::cerr << "[Encoder] invalid BGR stride: step=" << bgr.step
                << " cols=" << bgr.cols << " elem_size=" << elem_size << std::endl;
      return false;
    }

    const int src_wstride = static_cast<int>(bgr.step / elem_size);
    const int src_hstride = bgr.rows;
    rga_buffer_t src = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(bgr.data), bgr.cols, bgr.rows, RK_FORMAT_BGR_888, src_wstride, src_hstride);
    rga_buffer_t dst = wrapbuffer_virtualaddr(
        nv12_buffer_.data(), cfg_.width, cfg_.height, RK_FORMAT_YCbCr_420_SP, cfg_.width, cfg_.height);

    IM_STATUS status = imcvtcolor(src, dst, src.format, dst.format);
    if (status != IM_STATUS_SUCCESS) {
      std::cerr << "[Encoder] RGA bgr->nv12 failed: " << static_cast<int>(status) << std::endl;
      return false;
    }
    return true;
  }

  bool encode_nv12(EncodedPacket& out_packet) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
      return false;
    }

    frame->format = AV_PIX_FMT_NV12;
    frame->width = cfg_.width;
    frame->height = cfg_.height;
    frame->pts = pts_++;

    int ret = av_image_fill_arrays(
        frame->data, frame->linesize, nv12_buffer_.data(), AV_PIX_FMT_NV12, cfg_.width, cfg_.height, 1);
    if (ret < 0) {
      av_frame_free(&frame);
      return false;
    }

    ret = avcodec_send_frame(ctx_, frame);
    av_frame_free(&frame);
    if (ret < 0) {
      std::cerr << "[Encoder] avcodec_send_frame failed: " << ff_err_str(ret) << std::endl;
      return false;
    }

    out_packet.payload.clear();
    out_packet.is_keyframe = false;
    out_packet.stamp_ns = now_ns();

    while (true) {
      ret = avcodec_receive_packet(ctx_, pkt_);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      }
      if (ret < 0) {
        std::cerr << "[Encoder] avcodec_receive_packet failed: " << ff_err_str(ret) << std::endl;
        return false;
      }

      if ((pkt_->flags & AV_PKT_FLAG_KEY) != 0) {
        out_packet.is_keyframe = true;
      }

      const size_t old_size = out_packet.payload.size();
      out_packet.payload.resize(old_size + static_cast<size_t>(pkt_->size));
      std::memcpy(out_packet.payload.data() + old_size, pkt_->data, static_cast<size_t>(pkt_->size));
      av_packet_unref(pkt_);
    }

    return !out_packet.payload.empty();
  }

 private:
  EncoderConfig cfg_;
  AVCodecContext* ctx_ = nullptr;
  const AVCodec* codec_ = nullptr;
  AVPacket* pkt_ = nullptr;
  std::vector<uint8_t> nv12_buffer_;
  int64_t pts_ = 0;
  bool initialized_ = false;
};

Encoder::Encoder() : impl_(new Impl()) {}

Encoder::~Encoder() {
  delete impl_;
  impl_ = nullptr;
}

bool Encoder::init(const EncoderConfig& config) {
  return impl_->init(config);
}

bool Encoder::encode_bgr(const uint8_t* bgr_data,
                         int width,
                         int height,
                         int stride_bytes,
                         EncodedPacket& out_packet) {
  return impl_->encode_bgr(bgr_data, width, height, stride_bytes, out_packet);
}

bool Encoder::encode_jpeg(const uint8_t* jpeg_data,
                          size_t jpeg_size,
                          EncodedPacket& out_packet) {
  return impl_->encode_jpeg(jpeg_data, jpeg_size, out_packet);
}

}  // namespace hwcodec_core
