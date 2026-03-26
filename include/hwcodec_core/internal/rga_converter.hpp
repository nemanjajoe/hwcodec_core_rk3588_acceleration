#pragma once

#include <opencv2/opencv.hpp>

namespace hwcodec_core {

class RgaConverter {
 public:
  int init();
  int nv12_dma_to_rgb(int src_fd,
                      int src_w,
                      int src_h,
                      int dst_w,
                      int dst_h,
                      cv::Mat& rgb_out);
  int nv12_dma_to_bgr(int src_fd,
                      int src_w,
                      int src_h,
                      int dst_w,
                      int dst_h,
                      cv::Mat& bgr_out);
};

}  // namespace hwcodec_core
