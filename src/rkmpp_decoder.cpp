#include "hwcodec_core/internal/rkmpp_decoder.hpp"

#include <rockchip/mpp_err.h>

#include <chrono>
#include <iostream>
#include <thread>

namespace hwcodec_core {

MppDecoder::MppDecoder() = default;

MppDecoder::~MppDecoder() {
  destroy();
}

void MppDecoder::destroy() {
  if (frame_) {
    mpp_frame_deinit(&frame_);
    frame_ = nullptr;
  }
  if (packet_) {
    mpp_packet_deinit(&packet_);
    packet_ = nullptr;
  }
  if (ctx_) {
    mpp_destroy(ctx_);
    ctx_ = nullptr;
  }
  mpi_ = nullptr;
}

int MppDecoder::init(int width,
                     int height,
                     int split_mode,
                     int output_timeout_ms,
                     int put_retry,
                     int get_retry,
                     int put_retry_sleep_ms,
                     int get_retry_sleep_ms,
                     bool debug) {
  width_ = width;
  height_ = height;
  split_mode_ = split_mode;
  output_timeout_ms_ = output_timeout_ms;
  put_retry_ = put_retry;
  get_retry_ = get_retry;
  put_retry_sleep_ms_ = put_retry_sleep_ms;
  get_retry_sleep_ms_ = get_retry_sleep_ms;
  debug_ = debug;

  if (put_retry_ <= 0 || get_retry_ <= 0 || put_retry_sleep_ms_ < 0 || get_retry_sleep_ms_ < 0) {
    std::cerr << "[MppDecoder] invalid retry config" << std::endl;
    return -1;
  }

  MPP_RET ret = mpp_create(&ctx_, &mpi_);
  if (ret != MPP_OK) {
    std::cerr << "[MppDecoder] mpp_create failed" << std::endl;
    return -1;
  }

  RK_S64 output_timeout = static_cast<RK_S64>(output_timeout_ms_);
  ret = mpi_->control(ctx_, MPP_SET_OUTPUT_TIMEOUT, &output_timeout);
  if (ret != MPP_OK && debug_) {
    std::cerr << "[MppDecoder] set output timeout failed, value=" << output_timeout_ms_ << std::endl;
  }

  RK_U32 split_mode_u32 = split_mode_ > 0 ? 1U : 0U;
  ret = mpi_->control(ctx_, MPP_DEC_SET_PARSER_SPLIT_MODE, &split_mode_u32);
  if (ret != MPP_OK) {
    std::cerr << "[MppDecoder] set split mode failed" << std::endl;
    return -1;
  }

  ret = mpp_init(ctx_, MPP_CTX_DEC, MPP_VIDEO_CodingHEVC);
  if (ret != MPP_OK) {
    std::cerr << "[MppDecoder] mpp_init failed" << std::endl;
    return -1;
  }

  ret = mpp_packet_init(&packet_, nullptr, 0);
  if (ret != MPP_OK) {
    std::cerr << "[MppDecoder] mpp_packet_init failed" << std::endl;
    return -1;
  }

  return 0;
}

int MppDecoder::decode(const uint8_t* data, size_t size) {
  if (!ctx_ || !mpi_ || !packet_ || !data || size == 0) {
    if (debug_) {
      std::cerr << "[MppDecoder][debug] invalid decode input/state" << std::endl;
    }
    return -1;
  }

  mpp_packet_set_data(packet_, const_cast<uint8_t*>(data));
  mpp_packet_set_size(packet_, size);
  mpp_packet_set_pos(packet_, const_cast<uint8_t*>(data));
  mpp_packet_set_length(packet_, size);
  mpp_packet_clr_eos(packet_);

  hor_stride_ = 0;
  ver_stride_ = 0;
  if (debug_) {
    std::cerr << "[MppDecoder][debug] decode start bytes=" << size
              << " timeout_ms=" << output_timeout_ms_
              << " put_retry=" << put_retry_
              << " get_retry=" << get_retry_ << std::endl;
  }

  if (frame_) {
    mpp_frame_deinit(&frame_);
    frame_ = nullptr;
  }

  constexpr int kRetry = -3;
  constexpr int kFatal = -2;

  auto get_one_frame = [this, kRetry, kFatal](int timeout_ms) -> int {
    RK_S64 timeout = static_cast<RK_S64>(timeout_ms);
    MPP_RET ctrl_ret = mpi_->control(ctx_, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    if (ctrl_ret != MPP_OK) {
      if (debug_) {
        std::cerr << "[MppDecoder][debug] set output timeout failed ret=" << ctrl_ret
                  << " timeout_ms=" << timeout_ms << std::endl;
      }
      return kFatal;
    }

    MPP_RET ret = mpi_->decode_get_frame(ctx_, &frame_);
    if (ret != MPP_OK) {
      // Follow FFmpeg rkmpp logic: treat no-frame timeout states as retryable.
      if (ret == MPP_NOK || ret == MPP_ERR_TIMEOUT) {
        return kRetry;
      }
      if (debug_) {
        std::cerr << "[MppDecoder][debug] decode_get_frame failed ret=" << ret << std::endl;
      }
      return kFatal;
    }

    if (!frame_) {
      return kRetry;
    }

    if (mpp_frame_get_info_change(frame_)) {
      if (debug_) {
        std::cerr << "[MppDecoder][debug] got info_change frame" << std::endl;
      }
      mpi_->control(ctx_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
      mpp_frame_deinit(&frame_);
      frame_ = nullptr;
      return kRetry;
    }

    if (mpp_frame_get_errinfo(frame_) || mpp_frame_get_discard(frame_)) {
      if (debug_) {
        std::cerr << "[MppDecoder][debug] drop frame errinfo="
                  << mpp_frame_get_errinfo(frame_)
                  << " discard=" << mpp_frame_get_discard(frame_) << std::endl;
      }
      mpp_frame_deinit(&frame_);
      frame_ = nullptr;
      return kRetry;
    }

    const int frame_w = mpp_frame_get_width(frame_);
    const int frame_h = mpp_frame_get_height(frame_);
    hor_stride_ = mpp_frame_get_hor_stride(frame_);
    ver_stride_ = mpp_frame_get_ver_stride(frame_);
    const int fmt = static_cast<int>(mpp_frame_get_fmt(frame_) & MPP_FRAME_FMT_MASK);
    const int eos = mpp_frame_get_eos(frame_);

    MppBuffer buffer = mpp_frame_get_buffer(frame_);
    if (!buffer) {
      if (debug_) {
        std::cerr << "[MppDecoder][debug] frame has no buffer" << std::endl;
      }
      mpp_frame_deinit(&frame_);
      frame_ = nullptr;
      return kRetry;
    }

    const int fd = mpp_buffer_get_fd(buffer);
    if (fd < 0 || hor_stride_ <= 0 || ver_stride_ <= 0) {
      if (debug_) {
        std::cerr << "[MppDecoder][debug] invalid frame output fd=" << fd
                  << " frame=" << frame_w << "x" << frame_h
                  << " stride=" << hor_stride_ << "x" << ver_stride_ << std::endl;
      }
      mpp_frame_deinit(&frame_);
      frame_ = nullptr;
      return kRetry;
    }

    if (debug_) {
      std::cerr << "[MppDecoder][debug] decode success fd=" << fd
                << " frame=" << frame_w << "x" << frame_h
                << " stride=" << hor_stride_ << "x" << ver_stride_
                << " fmt=" << fmt
                << " eos=" << eos << std::endl;
    }
    return fd;
  };

  bool packet_sent = false;
  int put_retry = 0;
  int get_retry = 0;

  while ((packet_sent || put_retry < put_retry_) && get_retry < get_retry_) {
    if (!packet_sent && put_retry < put_retry_) {
      MPP_RET put_ret = mpi_->decode_put_packet(ctx_, packet_);
      if (put_ret == MPP_OK) {
        if (debug_) {
          std::cerr << "[MppDecoder][debug] decode_put_packet ok" << std::endl;
        }
        packet_sent = true;
      } else if (put_ret == MPP_ERR_BUFFER_FULL || put_ret == MPP_NOK) {
        ++put_retry;
        if (debug_) {
          std::cerr << "[MppDecoder][debug] decode_put_packet retry ret=" << put_ret
                    << " retry=" << put_retry << "/" << put_retry_ << std::endl;
        }
        if (put_retry_sleep_ms_ > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(put_retry_sleep_ms_));
        }
        continue;
      } else {
        if (debug_) {
          std::cerr << "[MppDecoder][debug] decode_put_packet failed ret=" << put_ret << std::endl;
        }
        return -1;
      }
    }

    const int frame_result = get_one_frame(output_timeout_ms_);
    if (frame_result >= 0) {
      return frame_result;
    }
    if (frame_result == kFatal) {
      return -1;
    }

    ++get_retry;
    if (debug_) {
      std::cerr << "[MppDecoder][debug] no frame yet, retry=" << get_retry
                << "/" << get_retry_ << std::endl;
    }
    if (get_retry_sleep_ms_ > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(get_retry_sleep_ms_));
    }
  }

  // Try FFmpeg-like EOS drain path.
  MppPacket eos_packet = nullptr;
  if (mpp_packet_init(&eos_packet, nullptr, 0) == MPP_OK && eos_packet) {
    mpp_packet_set_eos(eos_packet);
    if (debug_) {
      std::cerr << "[MppDecoder][debug] sending EOS packet for drain" << std::endl;
    }
    int eos_retry = 0;
    while (eos_retry < put_retry_) {
      MPP_RET eos_ret = mpi_->decode_put_packet(ctx_, eos_packet);
      if (eos_ret == MPP_OK) {
        if (debug_) {
          std::cerr << "[MppDecoder][debug] EOS packet accepted" << std::endl;
        }
        break;
      }
      if (eos_ret == MPP_ERR_BUFFER_FULL || eos_ret == MPP_NOK) {
        ++eos_retry;
        if (debug_) {
          std::cerr << "[MppDecoder][debug] EOS put retry ret=" << eos_ret
                    << " retry=" << eos_retry << "/" << put_retry_ << std::endl;
        }
        if (put_retry_sleep_ms_ > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(put_retry_sleep_ms_));
        }
        continue;
      }
      if (debug_) {
        std::cerr << "[MppDecoder][debug] EOS put failed ret=" << eos_ret << std::endl;
      }
      break;
    }
    mpp_packet_deinit(&eos_packet);
  }

  for (int i = 0; i < get_retry_; ++i) {
    const int frame_result = get_one_frame(MPP_TIMEOUT_MAX);
    if (frame_result >= 0) {
      return frame_result;
    }
    if (frame_result == kFatal) {
      return -1;
    }
    if (debug_) {
      std::cerr << "[MppDecoder][debug] drain retry=" << (i + 1) << "/" << get_retry_ << std::endl;
    }
  }

  if (debug_) {
    std::cerr << "[MppDecoder][debug] decode failed after send/get + EOS drain" << std::endl;
  }
  return -1;
}

}  // namespace hwcodec_core
