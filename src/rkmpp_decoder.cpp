#include "hwcodec_core/internal/rkmpp_decoder.hpp"

#include <rockchip/mpp_err.h>

#include <chrono>
#include <iostream>
#include <thread>

namespace hwcodec_core {

namespace {
constexpr int kMaxPutRetry = 5;
constexpr int kMaxGetRetry = 15;
}

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

int MppDecoder::init(int width, int height) {
  width_ = width;
  height_ = height;

  MPP_RET ret = mpp_create(&ctx_, &mpi_);
  if (ret != MPP_OK) {
    std::cerr << "[MppDecoder] mpp_create failed" << std::endl;
    return -1;
  }

  RK_U32 split_mode = 1;
  ret = mpi_->control(ctx_, MPP_DEC_SET_PARSER_SPLIT_MODE, &split_mode);
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
    return -1;
  }

  mpp_packet_set_data(packet_, const_cast<uint8_t*>(data));
  mpp_packet_set_size(packet_, size);
  mpp_packet_set_pos(packet_, const_cast<uint8_t*>(data));
  mpp_packet_set_length(packet_, size);

  int retry = 0;
  while (retry < kMaxPutRetry) {
    MPP_RET ret = mpi_->decode_put_packet(ctx_, packet_);
    if (ret == MPP_OK) {
      break;
    }
    if (ret == MPP_ERR_BUFFER_FULL) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      ++retry;
      continue;
    }
    return -1;
  }
  if (retry >= kMaxPutRetry) {
    return -1;
  }

  if (frame_) {
    mpp_frame_deinit(&frame_);
    frame_ = nullptr;
  }

  for (int get_retry = 0; get_retry < kMaxGetRetry; ++get_retry) {
    MPP_RET ret = mpi_->decode_get_frame(ctx_, &frame_);
    if (ret != MPP_OK) {
      return -1;
    }
    if (!frame_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
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

    hor_stride_ = mpp_frame_get_hor_stride(frame_);
    ver_stride_ = mpp_frame_get_ver_stride(frame_);

    MppBuffer buffer = mpp_frame_get_buffer(frame_);
    if (!buffer) {
      mpp_frame_deinit(&frame_);
      frame_ = nullptr;
      continue;
    }

    return mpp_buffer_get_fd(buffer);
  }

  return -1;
}

}  // namespace hwcodec_core
