#pragma once

#include <cstddef>
#include <cstdint>

#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>

namespace hwcodec_core {

class MppDecoder {
 public:
  MppDecoder();
  ~MppDecoder();

  int init(int width, int height);
  int decode(const uint8_t* data, size_t size);
  int get_hor_stride() const { return hor_stride_; }
  int get_ver_stride() const { return ver_stride_; }

 private:
  void destroy();

  int width_ = 0;
  int height_ = 0;
  int hor_stride_ = 0;
  int ver_stride_ = 0;

  MppCtx ctx_ = nullptr;
  MppApi* mpi_ = nullptr;
  MppPacket packet_ = nullptr;
  MppFrame frame_ = nullptr;
};

}  // namespace hwcodec_core
