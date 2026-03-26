#include "hwcodec_core/internal/rga_converter.hpp"

#include <rga/RgaApi.h>
#include <rga/im2d.h>

namespace hwcodec_core {

int RgaConverter::init() {
  return 0;
}

int RgaConverter::nv12_dma_to_rgb(int src_fd,
                                  int src_w,
                                  int src_h,
                                  int dst_w,
                                  int dst_h,
                                  cv::Mat& rgb_out) {
  if (rgb_out.empty() || rgb_out.cols != dst_w || rgb_out.rows != dst_h) {
    rgb_out.create(dst_h, dst_w, CV_8UC3);
  }

  rga_buffer_t src = wrapbuffer_fd(src_fd, src_w, src_h, RK_FORMAT_YCbCr_420_SP, src_w, src_h);
  rga_buffer_t dst = wrapbuffer_virtualaddr(rgb_out.data, dst_w, dst_h, RK_FORMAT_RGB_888, dst_w, dst_h);

  IM_STATUS status = imcvtcolor(src, dst, src.format, dst.format);
  if (status != IM_STATUS_SUCCESS) {
    return -1;
  }

  return 0;
}

int RgaConverter::nv12_dma_to_bgr(int src_fd,
                                  int src_w,
                                  int src_h,
                                  int dst_w,
                                  int dst_h,
                                  cv::Mat& bgr_out) {
  if (bgr_out.empty() || bgr_out.cols != dst_w || bgr_out.rows != dst_h) {
    bgr_out.create(dst_h, dst_w, CV_8UC3);
  }

  rga_buffer_t src = wrapbuffer_fd(src_fd, src_w, src_h, RK_FORMAT_YCbCr_420_SP, src_w, src_h);
  rga_buffer_t dst = wrapbuffer_virtualaddr(bgr_out.data, dst_w, dst_h, RK_FORMAT_BGR_888, dst_w, dst_h);

  IM_STATUS status = imcvtcolor(src, dst, src.format, dst.format);
  if (status != IM_STATUS_SUCCESS) {
    return -1;
  }

  return 0;
}

}  // namespace hwcodec_core
