#include "hwcodec_core/encoder.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool get_arg(int argc, char** argv, const std::string& key, std::string& out) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == key) {
      out = argv[i + 1];
      return true;
    }
  }
  return false;
}

int get_arg_int(int argc, char** argv, const std::string& key, int default_value) {
  std::string value;
  if (!get_arg(argc, argv, key, value)) {
    return default_value;
  }
  return std::stoi(value);
}

}  // namespace

int main(int argc, char** argv) {
  std::string input_path;
  std::string output_path;
  if (!get_arg(argc, argv, "--input", input_path) || !get_arg(argc, argv, "--output", output_path)) {
    std::cerr << "Usage: hw_encode_jpeg --input in.jpg --output out.h265 "
              << "[--width 1920 --height 1080 --fps 25 --debug 1 "
              << "--qp-min 10 --qp-max 48 "
              << "--prefer-mpp-jpeg 1 --jpeg-mpp-timeout-ms 0 "
              << "--jpeg-mpp-put-retry 5 --jpeg-mpp-get-retry 20 "
              << "--jpeg-mpp-put-sleep-ms 1 --jpeg-mpp-get-sleep-ms 1 "
              << "--jpeg-mpp-eos 0]" << std::endl;
    return 1;
  }

  std::ifstream ifs(input_path, std::ios::binary);
  if (!ifs) {
    std::cerr << "open input failed: " << input_path << std::endl;
    return 1;
  }
  std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

  hwcodec_core::EncoderConfig cfg;
  cfg.width = get_arg_int(argc, argv, "--width", 1920);
  cfg.height = get_arg_int(argc, argv, "--height", 1080);
  cfg.fps = get_arg_int(argc, argv, "--fps", 25);
  cfg.qp_min = get_arg_int(argc, argv, "--qp-min", 10);
  cfg.qp_max = get_arg_int(argc, argv, "--qp-max", 48);
  cfg.prefer_mpp_jpeg_decoder = (get_arg_int(argc, argv, "--prefer-mpp-jpeg", 1) != 0);
  cfg.jpeg_mpp_output_timeout_ms = get_arg_int(argc, argv, "--jpeg-mpp-timeout-ms", 0);
  cfg.jpeg_mpp_put_retry = get_arg_int(argc, argv, "--jpeg-mpp-put-retry", 5);
  cfg.jpeg_mpp_get_retry = get_arg_int(argc, argv, "--jpeg-mpp-get-retry", 20);
  cfg.jpeg_mpp_put_retry_sleep_ms = get_arg_int(argc, argv, "--jpeg-mpp-put-sleep-ms", 1);
  cfg.jpeg_mpp_get_retry_sleep_ms = get_arg_int(argc, argv, "--jpeg-mpp-get-sleep-ms", 1);
  cfg.jpeg_mpp_set_packet_eos = (get_arg_int(argc, argv, "--jpeg-mpp-eos", 0) != 0);
  cfg.debug = (get_arg_int(argc, argv, "--debug", 0) != 0);

  hwcodec_core::Encoder encoder;
  if (!encoder.init(cfg)) {
    std::cerr << "encoder init failed" << std::endl;
    return 1;
  }

  hwcodec_core::EncodedPacket packet;
  if (!encoder.encode_jpeg(jpeg.data(), jpeg.size(), packet)) {
    std::cerr << "encode returned no packet, try flush delayed packets..." << std::endl;
    if (!encoder.flush(packet)) {
      std::cerr << "encode failed" << std::endl;
      return 1;
    }
  }

  std::ofstream ofs(output_path, std::ios::binary);
  if (!ofs) {
    std::cerr << "open output failed: " << output_path << std::endl;
    return 1;
  }
  ofs.write(reinterpret_cast<const char*>(packet.payload.data()), static_cast<std::streamsize>(packet.payload.size()));

  std::cout << "encoded bytes=" << packet.payload.size() << " keyframe=" << packet.is_keyframe << std::endl;
  return 0;
}
