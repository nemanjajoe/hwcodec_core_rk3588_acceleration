#include "hwcodec_core/decoder.hpp"

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

bool has_flag(int argc, char** argv, const std::string& key) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == key) {
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
    std::cerr << "Usage: hw_decode_h265 --input in.h265 --output out.jpg "
              << "[--width 1920 --height 1080 --quality 80 "
              << "--mpp-split-mode 1 --mpp-timeout-ms 0 "
              << "--mpp-put-retry 5 --mpp-get-retry 15 "
              << "--mpp-put-sleep-ms 2 --mpp-get-sleep-ms 1 --debug 0]" << std::endl;
    return 1;
  }

  std::ifstream ifs(input_path, std::ios::binary);
  if (!ifs) {
    std::cerr << "open input failed: " << input_path << std::endl;
    return 1;
  }
  std::vector<uint8_t> h265((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

  hwcodec_core::DecoderConfig cfg;
  cfg.width = get_arg_int(argc, argv, "--width", 1920);
  cfg.height = get_arg_int(argc, argv, "--height", 1080);
  cfg.jpeg_quality = get_arg_int(argc, argv, "--quality", 80);
  cfg.mpp_split_mode = get_arg_int(argc, argv, "--mpp-split-mode", 1);
  cfg.mpp_output_timeout_ms = get_arg_int(argc, argv, "--mpp-timeout-ms", 0);
  cfg.mpp_put_retry = get_arg_int(argc, argv, "--mpp-put-retry", 5);
  cfg.mpp_get_retry = get_arg_int(argc, argv, "--mpp-get-retry", 15);
  cfg.mpp_put_retry_sleep_ms = get_arg_int(argc, argv, "--mpp-put-sleep-ms", 2);
  cfg.mpp_get_retry_sleep_ms = get_arg_int(argc, argv, "--mpp-get-sleep-ms", 1);
  cfg.debug = has_flag(argc, argv, "--debug") || (get_arg_int(argc, argv, "--debug", 0) != 0);

  hwcodec_core::Decoder decoder;
  if (!decoder.init(cfg)) {
    std::cerr << "decoder init failed" << std::endl;
    return 1;
  }

  hwcodec_core::EncodedPacket packet;
  packet.payload = std::move(h265);

  std::vector<uint8_t> jpeg;
  if (!decoder.decode_to_jpeg(packet, jpeg)) {
    std::cerr << "decode failed" << std::endl;
    return 1;
  }

  std::ofstream ofs(output_path, std::ios::binary);
  if (!ofs) {
    std::cerr << "open output failed: " << output_path << std::endl;
    return 1;
  }
  ofs.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));

  std::cout << "decoded jpeg bytes=" << jpeg.size() << std::endl;
  return 0;
}
