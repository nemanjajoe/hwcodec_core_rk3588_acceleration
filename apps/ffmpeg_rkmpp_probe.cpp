#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

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

std::string sh_quote(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('\'');
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

struct StepResult {
  int code = -1;
  double elapsed_ms = 0.0;
};

StepResult run_step(const std::string& name, const std::string& cmd, bool verbose) {
  std::cout << "[Probe] " << name << std::endl;
  if (verbose) {
    std::cout << "[Probe] cmd: " << cmd << std::endl;
  }

  const auto t0 = std::chrono::steady_clock::now();
  const int rc = std::system(cmd.c_str());
  const auto t1 = std::chrono::steady_clock::now();

  StepResult result;
  result.code = rc;
  result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  std::cout << "[Probe] result=" << (rc == 0 ? "PASS" : "FAIL")
            << " code=" << rc
            << " elapsed_ms=" << result.elapsed_ms << std::endl;
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  std::string input_path;
  std::string output_path;
  std::string ffmpeg_bin = "ffmpeg";

  if (!get_arg(argc, argv, "--input", input_path) || !get_arg(argc, argv, "--output", output_path)) {
    std::cerr << "Usage: ffmpeg_rkmpp_probe --input in.jpg --output out.h265 "
              << "[--ffmpeg ffmpeg --width 1920 --height 1080 --fps 25 --verbose 0]"
              << std::endl;
    return 1;
  }

  get_arg(argc, argv, "--ffmpeg", ffmpeg_bin);
  const int width = get_arg_int(argc, argv, "--width", 1920);
  const int height = get_arg_int(argc, argv, "--height", 1080);
  const int fps = get_arg_int(argc, argv, "--fps", 25);
  const bool verbose = get_arg_int(argc, argv, "--verbose", 0) != 0;

  if (width <= 0 || height <= 0 || fps <= 0) {
    std::cerr << "[Probe] invalid width/height/fps" << std::endl;
    return 1;
  }

  const std::string ff = sh_quote(ffmpeg_bin);
  const std::string in = sh_quote(input_path);
  const std::string out = sh_quote(output_path);

  std::ostringstream decode_cmd;
  decode_cmd << ff
             << " -hide_banner -loglevel info -y"
             << " -hwaccel rkmpp -hwaccel_output_format drm_prime"
             << " -i " << in
             << " -frames:v 1 -f null -";

  std::ostringstream encode_cmd;
  encode_cmd << ff
             << " -hide_banner -loglevel info -y"
             << " -i " << in
             << " -frames:v 1"
             << " -vf " << sh_quote("fps=" + std::to_string(fps) + ",scale=" +
                                     std::to_string(width) + ":" + std::to_string(height) + ",format=nv12")
             << " -c:v hevc_rkmpp -f hevc " << out;

  std::cout << "[Probe] Start ffmpeg rkmpp capability probe" << std::endl;
  StepResult dec = run_step("JPEG hw decode (mjpeg_rkmpp)", decode_cmd.str(), verbose);
  StepResult enc = run_step("HEVC hw encode (hevc_rkmpp)", encode_cmd.str(), verbose);

  std::cout << "[Probe] Summary: decode=" << (dec.code == 0 ? "PASS" : "FAIL")
            << " encode=" << (enc.code == 0 ? "PASS" : "FAIL") << std::endl;

  if (dec.code != 0) {
    std::cout << "[Probe] hint: mjpeg_rkmpp path failed; check ffmpeg logs, permissions, or JPEG format support."
              << std::endl;
  }
  if (enc.code != 0) {
    std::cout << "[Probe] hint: hevc_rkmpp path failed; check MPP runtime/driver and encoder parameters."
              << std::endl;
  }

  return (dec.code == 0 && enc.code == 0) ? 0 : 2;
}
