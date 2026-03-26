#include "hwcodec_core/encoder.hpp"

#include <opencv2/opencv.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <rga/im2d.h>

extern "C" {
#include <libavcodec/bsf.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>
}

namespace hwcodec_core {

namespace {

struct MppJpegDecoderOptions {
  RK_S64 output_timeout_ms = MPP_TIMEOUT_NON_BLOCK;
  int expected_width = 1920;
  int expected_height = 1080;
  int put_retry = 5;
  int get_retry = 20;
  int put_retry_sleep_ms = 1;
  int get_retry_sleep_ms = 1;
  bool set_packet_eos = false;
  bool debug = false;
};

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

bool debug_from_env(const char* name) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return false;
  }
  return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
         std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 ||
         std::strcmp(value, "ON") == 0;
}

class MppJpegDecoder {
 public:
  ~MppJpegDecoder() { destroy(); }

  bool init(const MppJpegDecoderOptions& options) {
    destroy();
    options_ = options;

    MPP_RET ret = mpp_create(&ctx_, &mpi_);
    if (ret != MPP_OK) {
      return false;
    }

    RK_S64 timeout = options_.output_timeout_ms;
    mpi_->control(ctx_, MPP_SET_OUTPUT_TIMEOUT, &timeout);

    ret = mpp_init(ctx_, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
    if (ret != MPP_OK) {
      destroy();
      return false;
    }

    MppBufferType buf_type = static_cast<MppBufferType>(MPP_BUFFER_TYPE_DRM |
                                                        MPP_BUFFER_FLAGS_DMA32 |
                                                        MPP_BUFFER_FLAGS_CACHABLE);
    ret = mpp_buffer_group_get_internal(&misc_group_, buf_type);
    if (ret != MPP_OK || !misc_group_) {
      ret = mpp_buffer_group_get_internal(&misc_group_, MPP_BUFFER_TYPE_DRM);
    }
    if (ret != MPP_OK || !misc_group_) {
      destroy();
      return false;
    }

    ready_ = true;
    return true;
  }

  bool ready() const { return ready_; }

  bool decode(const uint8_t* jpeg_data, size_t jpeg_size) {
    if (!ready_ || !ctx_ || !mpi_ || !misc_group_ || !jpeg_data || jpeg_size == 0) {
      return false;
    }

    if (frame_) {
      mpp_frame_deinit(&frame_);
      frame_ = nullptr;
    }

    if (pending_packet_) {
      mpp_packet_deinit(&pending_packet_);
      pending_packet_ = nullptr;
    }

    MppBuffer pkt_buf = nullptr;
    MppBuffer out_buf = nullptr;
    MppPacket packet = nullptr;
    MppFrame out_frame = nullptr;

    auto cleanup = [&]() {
      if (out_frame) {
        mpp_frame_deinit(&out_frame);
        out_frame = nullptr;
      }
      if (packet) {
        mpp_packet_deinit(&packet);
        packet = nullptr;
      }
      if (pkt_buf) {
        mpp_buffer_put(pkt_buf);
        pkt_buf = nullptr;
      }
      if (out_buf) {
        mpp_buffer_put(out_buf);
        out_buf = nullptr;
      }
    };

    if (mpp_buffer_get(misc_group_, &pkt_buf, jpeg_size) != MPP_OK || !pkt_buf) {
      cleanup();
      return false;
    }
    if (mpp_buffer_write(pkt_buf, 0, const_cast<uint8_t*>(jpeg_data), jpeg_size) != MPP_OK) {
      cleanup();
      return false;
    }
    (void)mpp_buffer_sync_partial_end(pkt_buf, 0, static_cast<RK_U32>(jpeg_size));

    if (mpp_packet_init_with_buffer(&packet, pkt_buf) != MPP_OK || !packet) {
      cleanup();
      return false;
    }
    mpp_buffer_put(pkt_buf);
    pkt_buf = nullptr;
    MppBuffer packet_buf = mpp_packet_get_buffer(packet);
    uint8_t* packet_ptr = packet_buf ? static_cast<uint8_t*>(mpp_buffer_get_ptr(packet_buf)) : nullptr;
    if (!packet_ptr) {
      cleanup();
      return false;
    }
    mpp_packet_set_data(packet, packet_ptr);
    mpp_packet_set_pos(packet, packet_ptr);
    mpp_packet_set_size(packet, jpeg_size);
    mpp_packet_set_length(packet, jpeg_size);
    if (options_.set_packet_eos) {
      mpp_packet_set_eos(packet);
    } else {
      mpp_packet_clr_eos(packet);
    }

    if (mpp_frame_init(&out_frame) != MPP_OK || !out_frame) {
      cleanup();
      return false;
    }
    const size_t out_w = static_cast<size_t>((options_.expected_width + 15) & ~15);
    const size_t out_h = static_cast<size_t>((options_.expected_height + 15) & ~15);
    const size_t out_size = out_w * out_h * 4U;
    if (mpp_buffer_get(misc_group_, &out_buf, out_size) != MPP_OK || !out_buf) {
      cleanup();
      return false;
    }
    mpp_frame_set_buffer(out_frame, out_buf);
    mpp_buffer_put(out_buf);
    out_buf = nullptr;

    MppMeta pkt_meta = mpp_packet_get_meta(packet);
    if (!pkt_meta || mpp_meta_set_frame(pkt_meta, KEY_OUTPUT_FRAME, out_frame) != MPP_OK) {
      cleanup();
      return false;
    }

    int put_retry = 0;
    while (put_retry < options_.put_retry) {
      MPP_RET ret = mpi_->decode_put_packet(ctx_, packet);
      if (ret == MPP_OK) {
        break;
      }
      if (ret == MPP_ERR_BUFFER_FULL) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options_.put_retry_sleep_ms));
        ++put_retry;
        continue;
      }
      cleanup();
      return false;
    }
    if (put_retry >= options_.put_retry) {
      cleanup();
      return false;
    }
    pending_packet_ = packet;
    packet = nullptr;
    mpp_frame_deinit(&out_frame);
    out_frame = nullptr;

    for (int get_retry = 0; get_retry < options_.get_retry; ++get_retry) {
      MPP_RET ret = mpi_->decode_get_frame(ctx_, &frame_);
      if (ret != MPP_OK) {
        cleanup();
        return false;
      }
      if (!frame_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options_.get_retry_sleep_ms));
        continue;
      }

      MppMeta frame_meta = mpp_frame_get_meta(frame_);
      if (frame_meta) {
        MppPacket packet_ret = nullptr;
        if (mpp_meta_get_packet(frame_meta, KEY_INPUT_PACKET, &packet_ret) == MPP_OK && packet_ret) {
          mpp_packet_deinit(&packet_ret);
          pending_packet_ = nullptr;
        }
      }

      if (mpp_frame_get_info_change(frame_)) {
        mpi_->control(ctx_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
        mpp_frame_deinit(&frame_);
        frame_ = nullptr;
        continue;
      }

      if (mpp_frame_get_errinfo(frame_) || mpp_frame_get_discard(frame_)) {
        mpp_frame_deinit(&frame_);
        frame_ = nullptr;
        continue;
      }

      MppBuffer buffer = mpp_frame_get_buffer(frame_);
      if (!buffer) {
        mpp_frame_deinit(&frame_);
        frame_ = nullptr;
        continue;
      }

      const auto fmt_masked = static_cast<MppFrameFormat>(mpp_frame_get_fmt(frame_) & MPP_FRAME_FMT_MASK);
      // Restrict to NV12 path for stable downstream RGA processing.
      if (fmt_masked != MPP_FMT_YUV420SP) {
        if (options_.debug) {
          std::cerr << "[MppJpegDecoder] unsupported fmt=" << static_cast<int>(fmt_masked) << std::endl;
        }
        mpp_frame_deinit(&frame_);
        frame_ = nullptr;
        cleanup();
        return false;
      }

      fd_ = mpp_buffer_get_fd(buffer);
      width_ = mpp_frame_get_width(frame_);
      height_ = mpp_frame_get_height(frame_);
      hor_stride_ = mpp_frame_get_hor_stride(frame_);
      ver_stride_ = mpp_frame_get_ver_stride(frame_);

      if (pending_packet_) {
        mpp_packet_deinit(&pending_packet_);
        pending_packet_ = nullptr;
      }
      return fd_ >= 0 && width_ > 0 && height_ > 0 && hor_stride_ > 0 && ver_stride_ > 0;
    }

    cleanup();
    return false;
  }

  int fd() const { return fd_; }
  int width() const { return width_; }
  int height() const { return height_; }
  int hor_stride() const { return hor_stride_; }
  int ver_stride() const { return ver_stride_; }

 private:
  void destroy() {
    if (frame_) {
      mpp_frame_deinit(&frame_);
      frame_ = nullptr;
    }
    if (pending_packet_) {
      mpp_packet_deinit(&pending_packet_);
      pending_packet_ = nullptr;
    }
    if (misc_group_) {
      mpp_buffer_group_put(misc_group_);
      misc_group_ = nullptr;
    }
    if (ctx_) {
      mpp_destroy(ctx_);
      ctx_ = nullptr;
    }
    mpi_ = nullptr;
    ready_ = false;
    fd_ = -1;
    width_ = 0;
    height_ = 0;
    hor_stride_ = 0;
    ver_stride_ = 0;
  }

 private:
  bool ready_ = false;
  MppJpegDecoderOptions options_;
  MppCtx ctx_ = nullptr;
  MppApi* mpi_ = nullptr;
  MppBufferGroup misc_group_ = nullptr;
  MppPacket pending_packet_ = nullptr;
  MppFrame frame_ = nullptr;

  int fd_ = -1;
  int width_ = 0;
  int height_ = 0;
  int hor_stride_ = 0;
  int ver_stride_ = 0;
};

}  // namespace

class Encoder::Impl {
 public:
  Impl() = default;

  ~Impl() {
    stop_worker();

    if (bsf_pkt_) {
      av_packet_free(&bsf_pkt_);
      bsf_pkt_ = nullptr;
    }
    if (bsf_ctx_) {
      av_bsf_free(&bsf_ctx_);
      bsf_ctx_ = nullptr;
    }

    if (pkt_) {
      av_packet_free(&pkt_);
      pkt_ = nullptr;
    }
    if (ctx_) {
      avcodec_free_context(&ctx_);
      ctx_ = nullptr;
    }

    if (dma_buffer_) {
      mpp_buffer_put(dma_buffer_);
      dma_buffer_ = nullptr;
    }
    if (dma_group_) {
      mpp_buffer_group_put(dma_group_);
      dma_group_ = nullptr;
    }
  }

  bool init(const EncoderConfig& config) {
    cfg_ = config;
    debug_enabled_ = cfg_.debug || debug_from_env("HWCODEC_DEBUG");
    if (cfg_.width <= 0 || cfg_.height <= 0 || cfg_.fps <= 0) {
      return false;
    }
    if (cfg_.qp_min < 0 || cfg_.qp_max < 0 || cfg_.qp_min > cfg_.qp_max) {
      std::cerr << "[Encoder] invalid qp range: qp_min=" << cfg_.qp_min
                << " qp_max=" << cfg_.qp_max << std::endl;
      return false;
    }
    if (cfg_.jpeg_mpp_put_retry <= 0 || cfg_.jpeg_mpp_get_retry <= 0 ||
        cfg_.jpeg_mpp_put_retry_sleep_ms < 0 || cfg_.jpeg_mpp_get_retry_sleep_ms < 0) {
      std::cerr << "[Encoder] invalid jpeg mpp retry config" << std::endl;
      return false;
    }

    if (debug_enabled_) {
      std::cerr << "[Encoder][debug] init with codec=" << cfg_.codec_name
                << " size=" << cfg_.width << "x" << cfg_.height
                << " fps=" << cfg_.fps
                << " bitrate=" << cfg_.bitrate
                << " gop=" << cfg_.gop
                << " bf=" << cfg_.bf
                << " rc_mode=" << cfg_.rc_mode
                << " profile=" << cfg_.profile
                << " qp_min=" << cfg_.qp_min
                << " qp_max=" << cfg_.qp_max
                << " prefer_mpp_jpeg_decoder=" << (cfg_.prefer_mpp_jpeg_decoder ? 1 : 0)
                << " jpeg_mpp_output_timeout_ms=" << cfg_.jpeg_mpp_output_timeout_ms
                << " jpeg_mpp_put_retry=" << cfg_.jpeg_mpp_put_retry
                << " jpeg_mpp_get_retry=" << cfg_.jpeg_mpp_get_retry
                << " jpeg_mpp_put_retry_sleep_ms=" << cfg_.jpeg_mpp_put_retry_sleep_ms
                << " jpeg_mpp_get_retry_sleep_ms=" << cfg_.jpeg_mpp_get_retry_sleep_ms
                << " jpeg_mpp_set_packet_eos=" << (cfg_.jpeg_mpp_set_packet_eos ? 1 : 0)
                << std::endl;
    }

    const size_t nv12_size = static_cast<size_t>(cfg_.width) * static_cast<size_t>(cfg_.height) * 3U / 2U;
    nv12_buffer_.resize(nv12_size);

    init_nv12_dma_buffer(nv12_size);

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
      const std::string qp_min = std::to_string(cfg_.qp_min);
      const std::string qp_max = std::to_string(cfg_.qp_max);
      av_dict_set(&opts, "rc_mode", "1", 0);
      av_dict_set(&opts, "qp_min", qp_min.c_str(), 0);
      av_dict_set(&opts, "qp_max", qp_max.c_str(), 0);
    } else {
      av_dict_set(&opts, "rc_mode", "0", 0);
    }

    int ret = avcodec_open2(ctx_, codec_, &opts);
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

    if (ctx_->codec_id == AV_CODEC_ID_HEVC) {
      if (!init_hevc_annexb_bsf()) {
        std::cerr << "[Encoder] failed to init hevc_mp4toannexb bsf" << std::endl;
        return false;
      }
    }

    if (cfg_.prefer_mpp_jpeg_decoder) {
      MppJpegDecoderOptions jpeg_opts;
      jpeg_opts.output_timeout_ms = static_cast<RK_S64>(cfg_.jpeg_mpp_output_timeout_ms);
      jpeg_opts.expected_width = cfg_.width;
      jpeg_opts.expected_height = cfg_.height;
      jpeg_opts.put_retry = cfg_.jpeg_mpp_put_retry;
      jpeg_opts.get_retry = cfg_.jpeg_mpp_get_retry;
      jpeg_opts.put_retry_sleep_ms = cfg_.jpeg_mpp_put_retry_sleep_ms;
      jpeg_opts.get_retry_sleep_ms = cfg_.jpeg_mpp_get_retry_sleep_ms;
      jpeg_opts.set_packet_eos = cfg_.jpeg_mpp_set_packet_eos;
      jpeg_opts.debug = debug_enabled_;
      if (!jpeg_hw_decoder_.init(jpeg_opts)) {
        std::cerr << "[Encoder] warning: MPP JPEG decoder init failed, fallback to cv::imdecode" << std::endl;
      }
    } else if (debug_enabled_) {
      std::cerr << "[Encoder] MPP JPEG decoder is disabled by config" << std::endl;
    }

    initialized_ = true;
    pts_ = 0;

    worker_ = std::thread(&Impl::worker_loop, this);
    return true;
  }

  
  bool encode_bgr(const uint8_t* bgr_data,
                  int width,
                  int height,
                  int stride_bytes,
                  EncodedPacket& out_packet) {
    if (!submit_bgr(bgr_data, width, height, stride_bytes)) {
      return false;
    }
    return receive_packet(out_packet);
  }

  bool encode_jpeg(const uint8_t* jpeg_data, size_t jpeg_size, EncodedPacket& out_packet) {
    if (!submit_jpeg(jpeg_data, jpeg_size)) {
      return false;
    }
    return receive_packet(out_packet);
  }

  bool submit_bgr(const uint8_t* bgr_data,
                  int width,
                  int height,
                  int stride_bytes) {
    if (!initialized_ || !bgr_data || width <= 0 || height <= 0 || stride_bytes <= 0) {
      return false;
    }

    EncodeTask task;
    task.type = EncodeTaskType::kBgr;
    task.width = width;
    task.height = height;
    task.stride_bytes = stride_bytes;
    const size_t bytes = static_cast<size_t>(stride_bytes) * static_cast<size_t>(height);
    task.data.assign(bgr_data, bgr_data + bytes);

    {
      std::lock_guard<std::mutex> lk(mutex_);
      task_queue_.push_back(std::move(task));
    }
    cv_task_.notify_one();
    return true;
  }

  bool submit_jpeg(const uint8_t* jpeg_data, size_t jpeg_size) {
    if (!initialized_ || !jpeg_data || jpeg_size == 0) {
      return false;
    }

    EncodeTask task;
    task.type = EncodeTaskType::kJpeg;
    task.data.assign(jpeg_data, jpeg_data + jpeg_size);

    {
      std::lock_guard<std::mutex> lk(mutex_);
      task_queue_.push_back(std::move(task));
    }
    cv_task_.notify_one();
    return true;
  }

  bool receive_packet(EncodedPacket& out_packet) {
    std::unique_lock<std::mutex> lk(mutex_);
    cv_result_.wait(lk, [this]() { return stop_worker_ || !result_queue_.empty(); });

    if (result_queue_.empty()) {
      return false;
    }

    EncodeResult result = std::move(result_queue_.front());
    result_queue_.pop_front();

    if (!result.ok) {
      return false;
    }

    out_packet = std::move(result.packet);
    return true;
  }

  bool flush(EncodedPacket& out_packet) {
    if (!ctx_ || !pkt_) {
      return false;
    }

    int ret = avcodec_send_frame(ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
      std::cerr << "[Encoder] flush avcodec_send_frame(null) failed: " << ff_err_str(ret) << std::endl;
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
        std::cerr << "[Encoder] flush avcodec_receive_packet failed: " << ff_err_str(ret) << std::endl;
        return false;
      }

      if (!append_packet_payload(pkt_, out_packet)) {
        av_packet_unref(pkt_);
        return false;
      }
      av_packet_unref(pkt_);
    }

    if (!drain_bsf(out_packet)) {
      return false;
    }

    if (debug_enabled_ && out_packet.payload.empty()) {
      std::cerr << "[Encoder] flush completed but no delayed packet is available" << std::endl;
    }
    return !out_packet.payload.empty();
  }

 private:
  enum class EncodeTaskType {
    kBgr,
    kJpeg,
  };

  struct EncodeTask {
    EncodeTaskType type = EncodeTaskType::kBgr;
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
    int stride_bytes = 0;
  };

  struct EncodeResult {
    bool ok = false;
    EncodedPacket packet;
  };

 private:
  void init_nv12_dma_buffer(size_t nv12_size) {
    MPP_RET ret = mpp_buffer_group_get_internal(&dma_group_, MPP_BUFFER_TYPE_DRM);
    if (ret != MPP_OK || !dma_group_) {
      use_dma_nv12_ = false;
      return;
    }

    ret = mpp_buffer_get(dma_group_, &dma_buffer_, nv12_size);
    if (ret != MPP_OK || !dma_buffer_) {
      mpp_buffer_group_put(dma_group_);
      dma_group_ = nullptr;
      use_dma_nv12_ = false;
      return;
    }

    dma_ptr_ = static_cast<uint8_t*>(mpp_buffer_get_ptr(dma_buffer_));
    dma_fd_ = mpp_buffer_get_fd(dma_buffer_);

    use_dma_nv12_ = (dma_ptr_ != nullptr && dma_fd_ >= 0);
    if (!use_dma_nv12_) {
      if (dma_buffer_) {
        mpp_buffer_put(dma_buffer_);
        dma_buffer_ = nullptr;
      }
      if (dma_group_) {
        mpp_buffer_group_put(dma_group_);
        dma_group_ = nullptr;
      }
      dma_ptr_ = nullptr;
      dma_fd_ = -1;
    }
  }

  void stop_worker() {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      stop_worker_ = true;
    }
    cv_task_.notify_all();
    cv_result_.notify_all();

    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void worker_loop() {
    while (true) {
      EncodeTask task;
      {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_task_.wait(lk, [this]() { return stop_worker_ || !task_queue_.empty(); });
        if (stop_worker_ && task_queue_.empty()) {
          return;
        }

        task = std::move(task_queue_.front());
        task_queue_.pop_front();
      }

      EncodeResult result;
      if (task.type == EncodeTaskType::kBgr) {
        result.ok = encode_bgr_impl(task.data.data(), task.width, task.height, task.stride_bytes, result.packet);
      } else {
        result.ok = encode_jpeg_impl(task.data.data(), task.data.size(), result.packet);
      }

      if (debug_enabled_ && !result.ok) {
        std::cerr << "[Encoder] worker encode task failed, type="
                  << (task.type == EncodeTaskType::kBgr ? "bgr" : "jpeg") << std::endl;
      }

      {
        std::lock_guard<std::mutex> lk(mutex_);
        result_queue_.push_back(std::move(result));
      }
      cv_result_.notify_one();
    }
  }

  bool encode_bgr_impl(const uint8_t* bgr_data,
                       int width,
                       int height,
                       int stride_bytes,
                       EncodedPacket& out_packet) {
    if (!bgr_to_nv12_target(bgr_data, width, height, stride_bytes)) {
      return false;
    }
    return encode_nv12(out_packet);
  }

  bool encode_jpeg_impl(const uint8_t* jpeg_data, size_t jpeg_size, EncodedPacket& out_packet) {
    bool prepared = false;

    if (jpeg_hw_decoder_.ready() && jpeg_hw_decoder_.decode(jpeg_data, jpeg_size)) {
      prepared = nv12_fd_to_nv12_target(jpeg_hw_decoder_.fd(),
                                        jpeg_hw_decoder_.width(),
                                        jpeg_hw_decoder_.height(),
                                        jpeg_hw_decoder_.hor_stride(),
                                        jpeg_hw_decoder_.ver_stride());
      if (debug_enabled_ && !prepared) {
        std::cerr << "[Encoder] MPP JPEG decode succeeded but NV12 fd->target conversion failed" << std::endl;
      }
    } else if (jpeg_hw_decoder_.ready()) {
      if (debug_enabled_) {
        std::cerr << "[Encoder] MPP JPEG decode failed, fallback to OpenCV decode" << std::endl;
      }
    }

    if (!prepared) {
      cv::Mat raw(1, static_cast<int>(jpeg_size), CV_8UC1, const_cast<uint8_t*>(jpeg_data));
      cv::Mat bgr = cv::imdecode(raw, cv::IMREAD_COLOR);
      if (bgr.empty()) {
        std::cerr << "[Encoder] jpeg decode failed on both MPP and OpenCV" << std::endl;
        return false;
      }
      prepared = bgr_to_nv12_target(bgr.data, bgr.cols, bgr.rows, static_cast<int>(bgr.step));
      if (debug_enabled_ && !prepared) {
        std::cerr << "[Encoder] OpenCV JPEG decode succeeded but BGR->NV12 conversion failed" << std::endl;
      }
    }

    if (!prepared) {
      return false;
    }

    return encode_nv12(out_packet);
  }

  bool bgr_to_nv12_target(const uint8_t* bgr_data,
                          int width,
                          int height,
                          int stride_bytes) {
    const size_t elem_size = 3;
    if (!bgr_data || width <= 0 || height <= 0 || stride_bytes <= 0) {
      if (debug_enabled_) {
        std::cerr << "[Encoder] invalid BGR input: ptr/size/stride check failed" << std::endl;
      }
      return false;
    }
    if (static_cast<size_t>(stride_bytes) < static_cast<size_t>(width) * elem_size) {
      if (debug_enabled_) {
        std::cerr << "[Encoder] invalid BGR stride: stride_bytes=" << stride_bytes
                  << " width=" << width << std::endl;
      }
      return false;
    }

    const int src_wstride = stride_bytes / static_cast<int>(elem_size);
    const int src_hstride = height;

    rga_buffer_t src = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(bgr_data), width, height, RK_FORMAT_BGR_888, src_wstride, src_hstride);

    rga_buffer_t nv12_dst = use_dma_nv12_
                                ? wrapbuffer_fd(dma_fd_, cfg_.width, cfg_.height, RK_FORMAT_YCbCr_420_SP,
                                                cfg_.width, cfg_.height)
                                : wrapbuffer_virtualaddr(nv12_buffer_.data(), cfg_.width, cfg_.height,
                                                         RK_FORMAT_YCbCr_420_SP, cfg_.width, cfg_.height);

    if (width == cfg_.width && height == cfg_.height) {
      IM_STATUS status = imcvtcolor(src, nv12_dst, src.format, nv12_dst.format);
      if (debug_enabled_ && status != IM_STATUS_SUCCESS) {
        std::cerr << "[Encoder] imcvtcolor failed, status=" << static_cast<int>(status)
                  << " src=" << width << "x" << height
                  << " dst=" << cfg_.width << "x" << cfg_.height
                  << " dma=" << (use_dma_nv12_ ? 1 : 0) << std::endl;
      }
      return status == IM_STATUS_SUCCESS;
    }

    const size_t resized_bytes = static_cast<size_t>(cfg_.width) * static_cast<size_t>(cfg_.height) * elem_size;
    if (tmp_bgr_resized_.size() != resized_bytes) {
      tmp_bgr_resized_.resize(resized_bytes);
    }

    rga_buffer_t bgr_resized = wrapbuffer_virtualaddr(tmp_bgr_resized_.data(), cfg_.width, cfg_.height,
                                                      RK_FORMAT_BGR_888, cfg_.width, cfg_.height);

    IM_STATUS status = imresize(src, bgr_resized);
    if (status != IM_STATUS_SUCCESS) {
      if (debug_enabled_) {
        std::cerr << "[Encoder] imresize failed, status=" << static_cast<int>(status)
                  << " src=" << width << "x" << height
                  << " dst=" << cfg_.width << "x" << cfg_.height << std::endl;
      }
      return false;
    }

    status = imcvtcolor(bgr_resized, nv12_dst, bgr_resized.format, nv12_dst.format);
    if (debug_enabled_ && status != IM_STATUS_SUCCESS) {
      std::cerr << "[Encoder] imcvtcolor(after resize) failed, status=" << static_cast<int>(status)
                << " dst=" << cfg_.width << "x" << cfg_.height
                << " dma=" << (use_dma_nv12_ ? 1 : 0) << std::endl;
    }
    return status == IM_STATUS_SUCCESS;
  }

  bool nv12_fd_to_nv12_target(int src_fd,
                              int src_w,
                              int src_h,
                              int src_wstride,
                              int src_hstride) {
    if (src_fd < 0 || src_w <= 0 || src_h <= 0 || src_wstride <= 0 || src_hstride <= 0) {
      if (debug_enabled_) {
        std::cerr << "[Encoder] invalid NV12 fd input: fd/size/stride check failed" << std::endl;
      }
      return false;
    }

    rga_buffer_t src = wrapbuffer_fd(src_fd, src_w, src_h, RK_FORMAT_YCbCr_420_SP, src_wstride, src_hstride);
    rga_buffer_t dst = use_dma_nv12_
                           ? wrapbuffer_fd(dma_fd_, cfg_.width, cfg_.height, RK_FORMAT_YCbCr_420_SP,
                                           cfg_.width, cfg_.height)
                           : wrapbuffer_virtualaddr(nv12_buffer_.data(), cfg_.width, cfg_.height,
                                                    RK_FORMAT_YCbCr_420_SP, cfg_.width, cfg_.height);

    IM_STATUS status;
    if (src_w == cfg_.width && src_h == cfg_.height) {
      status = imcopy(src, dst);
    } else {
      status = imresize(src, dst);
    }

    if (debug_enabled_ && status != IM_STATUS_SUCCESS) {
      std::cerr << "[Encoder] NV12 fd conversion failed, status=" << static_cast<int>(status)
                << " src=" << src_w << "x" << src_h
                << " stride=" << src_wstride << "x" << src_hstride
                << " dst=" << cfg_.width << "x" << cfg_.height
                << " dma=" << (use_dma_nv12_ ? 1 : 0) << std::endl;
    }

    return status == IM_STATUS_SUCCESS;
  }

  bool encode_nv12(EncodedPacket& out_packet) {
    if (!ctx_ || !pkt_) {
      return false;
    }

    uint8_t* nv12_ptr = use_dma_nv12_ ? dma_ptr_ : nv12_buffer_.data();
    if (!nv12_ptr) {
      if (debug_enabled_) {
        std::cerr << "[Encoder] NV12 source buffer is null" << std::endl;
      }
      return false;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
      return false;
    }

    frame->format = AV_PIX_FMT_NV12;
    frame->width = cfg_.width;
    frame->height = cfg_.height;
    frame->pts = pts_++;

    int ret = av_image_fill_arrays(frame->data, frame->linesize, nv12_ptr,
                                   AV_PIX_FMT_NV12, cfg_.width, cfg_.height, 1);
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
        if (debug_enabled_ && out_packet.payload.empty()) {
          std::cerr << "[Encoder] avcodec_receive_packet returned "
                    << (ret == AVERROR(EAGAIN) ? "EAGAIN" : "EOF")
                    << " before any packet was produced (encoder may be buffering; try sending next frame or flush)"
                    << std::endl;
        }
        break;
      }
      if (ret < 0) {
        std::cerr << "[Encoder] avcodec_receive_packet failed: " << ff_err_str(ret) << std::endl;
        return false;
      }

      if (!append_packet_payload(pkt_, out_packet)) {
        av_packet_unref(pkt_);
        return false;
      }
      av_packet_unref(pkt_);
    }

    return !out_packet.payload.empty();
  }

  bool init_hevc_annexb_bsf() {
    const AVBitStreamFilter* bsf = av_bsf_get_by_name("hevc_mp4toannexb");
    if (!bsf) {
      return false;
    }

    int ret = av_bsf_alloc(bsf, &bsf_ctx_);
    if (ret < 0 || !bsf_ctx_) {
      return false;
    }

    ret = avcodec_parameters_from_context(bsf_ctx_->par_in, ctx_);
    if (ret < 0) {
      av_bsf_free(&bsf_ctx_);
      return false;
    }
    bsf_ctx_->time_base_in = ctx_->time_base;

    ret = av_bsf_init(bsf_ctx_);
    if (ret < 0) {
      av_bsf_free(&bsf_ctx_);
      return false;
    }

    bsf_pkt_ = av_packet_alloc();
    if (!bsf_pkt_) {
      av_bsf_free(&bsf_ctx_);
      return false;
    }

    if (debug_enabled_) {
      std::cerr << "[Encoder][debug] hevc_mp4toannexb bsf enabled" << std::endl;
    }
    return true;
  }

  bool append_raw_packet(const AVPacket* packet, EncodedPacket& out_packet) {
    if (!packet || packet->size <= 0 || !packet->data) {
      return false;
    }

    if ((packet->flags & AV_PKT_FLAG_KEY) != 0) {
      out_packet.is_keyframe = true;
    }

    const size_t old_size = out_packet.payload.size();
    out_packet.payload.resize(old_size + static_cast<size_t>(packet->size));
    std::memcpy(out_packet.payload.data() + old_size, packet->data, static_cast<size_t>(packet->size));
    return true;
  }

  bool append_packet_payload(const AVPacket* packet, EncodedPacket& out_packet) {
    if (!bsf_ctx_) {
      return append_raw_packet(packet, out_packet);
    }

    int ret = av_bsf_send_packet(bsf_ctx_, const_cast<AVPacket*>(packet));
    if (ret < 0) {
      if (debug_enabled_) {
        std::cerr << "[Encoder][debug] av_bsf_send_packet failed: " << ff_err_str(ret) << std::endl;
      }
      return false;
    }

    while (true) {
      ret = av_bsf_receive_packet(bsf_ctx_, bsf_pkt_);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      }
      if (ret < 0) {
        if (debug_enabled_) {
          std::cerr << "[Encoder][debug] av_bsf_receive_packet failed: " << ff_err_str(ret) << std::endl;
        }
        return false;
      }

      if (!append_raw_packet(bsf_pkt_, out_packet)) {
        av_packet_unref(bsf_pkt_);
        return false;
      }
      av_packet_unref(bsf_pkt_);
    }

    return true;
  }

  bool drain_bsf(EncodedPacket& out_packet) {
    if (!bsf_ctx_) {
      return true;
    }

    int ret = av_bsf_send_packet(bsf_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
      if (debug_enabled_) {
        std::cerr << "[Encoder][debug] av_bsf_send_packet(null) failed: " << ff_err_str(ret) << std::endl;
      }
      return false;
    }

    while (true) {
      ret = av_bsf_receive_packet(bsf_ctx_, bsf_pkt_);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      }
      if (ret < 0) {
        if (debug_enabled_) {
          std::cerr << "[Encoder][debug] av_bsf_receive_packet(drain) failed: " << ff_err_str(ret) << std::endl;
        }
        return false;
      }
      if (!append_raw_packet(bsf_pkt_, out_packet)) {
        av_packet_unref(bsf_pkt_);
        return false;
      }
      av_packet_unref(bsf_pkt_);
    }
    return true;
  }

 private:
  EncoderConfig cfg_;

  AVCodecContext* ctx_ = nullptr;
  const AVCodec* codec_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVBSFContext* bsf_ctx_ = nullptr;
  AVPacket* bsf_pkt_ = nullptr;

  std::vector<uint8_t> nv12_buffer_;
  std::vector<uint8_t> tmp_bgr_resized_;

  MppBufferGroup dma_group_ = nullptr;
  MppBuffer dma_buffer_ = nullptr;
  uint8_t* dma_ptr_ = nullptr;
  int dma_fd_ = -1;
  bool use_dma_nv12_ = false;

  MppJpegDecoder jpeg_hw_decoder_;

  int64_t pts_ = 0;
  bool initialized_ = false;
  bool debug_enabled_ = false;

  std::mutex mutex_;
  std::condition_variable cv_task_;
  std::condition_variable cv_result_;
  std::deque<EncodeTask> task_queue_;
  std::deque<EncodeResult> result_queue_;
  bool stop_worker_ = false;
  std::thread worker_;
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

bool Encoder::submit_bgr(const uint8_t* bgr_data,
                         int width,
                         int height,
                         int stride_bytes) {
  return impl_->submit_bgr(bgr_data, width, height, stride_bytes);
}

bool Encoder::submit_jpeg(const uint8_t* jpeg_data, size_t jpeg_size) {
  return impl_->submit_jpeg(jpeg_data, jpeg_size);
}

bool Encoder::receive_packet(EncodedPacket& out_packet) {
  return impl_->receive_packet(out_packet);
}

bool Encoder::flush(EncodedPacket& out_packet) {
  return impl_->flush(out_packet);
}

}  // namespace hwcodec_core
