# hwcodec_core - Rockchip Hardware Codec Core Library

**Language**: [English](#english) | [中文](#中文)

---

<a name="english"></a>

## English

## Overview

**hwcodec_core** is a decoupled, hardware-accelerated encoding/decoding library for Rockchip platforms (RK3588 and compatible chips). It provides high-performance H.265 codec support and JPEG handling through native hardware acceleration with optional software fallback.

### Key Features

- **H.265 Encoding**: BGR/JPEG → H.265 (FFmpeg Rockchip encoder)
- **H.265 Decoding**: H.265 → BGR/JPEG (MPP + RGA)
- **Hardware Acceleration**: Leverages Rockchip MPP and RGA
- **Graceful Fallback**: Automatic switch to FFmpeg software codec on hardware failure
- **Standalone CMake Build**: No ROS dependency required
- **Production-Ready**: Used in ROS integration with stable interface contract

### Use Cases

- Embedded vision systems with real-time video encoding/decoding
- Edge AI inference with video preprocessing
- Video streaming with hardware acceleration on Rockchip boards
- Batch video processing pipelines

---

## 1. Documentation

### 1.1 Purpose

This document explains the design, directory structure, build process, API usage, CLI tools, deployment recommendations, and troubleshooting for the `hwcodec_core` module.

### 1.2 Scope

Current version targets Rockchip platforms for H.265 codec scenarios:

- **H.265 Encoding**: `BGR/JPEG → H.265`
- **H.265 Decoding**: `H.265 → BGR/JPEG`
- **Independent CMake Build**: No ROS dependency

---

## 2. Architecture Overview

### 2.1 Design Goals

Decouple hardware codec business logic from ROS nodes to create a reusable core library.

**Benefits**:

- ROS nodes remain lightweight (only message bridging)
- Codec logic can be independently tested and deployed
- Centralized dependency management, reduced system coupling

### 2.2 Core Capabilities

#### `hwcodec_core::Encoder`
- **Input**: BGR raw image or JPEG byte stream
- **Output**: H.265 bitstream packets (`EncodedPacket`)

#### `hwcodec_core::Decoder`
- **Input**: H.265 bitstream packets (`EncodedPacket`)
- **Output**: BGR image or JPEG byte stream

---

## 3. Project Structure

```text
hwcodec_core/
  CMakeLists.txt
  README.md
  README_CN.md
  build.sh
  apps/
    hw_encode_jpeg.cpp
    hw_decode_h265.cpp
    ffmpeg_rkmpp_probe.cpp
  include/
    hwcodec_core/
      types.hpp
      encoder.hpp
      decoder.hpp
      internal/
        rkmpp_decoder.hpp
        rga_converter.hpp
  src/
    encoder.cpp
    decoder.cpp
    rkmpp_decoder.cpp
    rga_converter.cpp
  cmake/
    (CMake modules)
  assets/
    (Assets)
```

---

## 4. Building and Installation

### 4.1 Requirements

- CMake >= 3.16
- C++14 compatible compiler
- OpenCV
- FFmpeg (with Rockchip `hevc_rkmpp` encoder)
- Rockchip MPP (`rockchip_mpp.pc`)
- Rockchip RGA (`librga.so`)

### 4.2 Build

```bash
cd hwcodec_core
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

### 4.3 Install (Optional)

```bash
cmake --install build
```

---

## 5. API Reference

### 5.1 Data Types

**Header**: `include/hwcodec_core/types.hpp`

```cpp
struct EncodedPacket {
  uint64_t stamp_ns = 0;        // Optional timestamp (nanoseconds)
  bool is_keyframe = false;      // Keyframe marker (IDR/CRA)
  std::vector<uint8_t> payload;  // HEVC bitstream bytes
};
```

**Payload Format**:
- Recommended: Annex-B (`00 00 01` or `00 00 00 01` start codes)
- Keyframes should carry VPS/SPS/PPS for downstream decoder initialization

### 5.2 Encoder API

**Header**: `include/hwcodec_core/encoder.hpp`

#### Lifecycle

```cpp
bool init(const EncoderConfig& config);
bool encode_bgr(const uint8_t* bgr_data, int width, int height, 
                int stride_bytes, EncodedPacket& out_packet);
bool encode_jpeg(const uint8_t* jpeg_data, size_t jpeg_size, 
                 EncodedPacket& out_packet);
```

**Thread Safety**: Single-thread per instance; separate instances for concurrent encode streams

**Parameters**:
- `bgr_data/jpeg_data`: Input buffer pointer
- `stride_bytes`: BGR row stride in bytes (must be >= width * 3)
- `out_packet`: Output packet (filled on success)

**Return**: `true` on successful encoding, `false` otherwise

**Important Notes**:
- `false` is not always fatal; internal buffering may prevent packet output
- *Do not* call `flush()` on every frame in real-time streams; use async pipeline for throughput optimization

#### Async Pipeline Interface

```cpp
bool submit_bgr(...);
bool submit_jpeg(...);
bool receive_packet(EncodedPacket& out_packet);
bool flush(EncodedPacket& out_packet);
```

### 5.3 Decoder API

**Header**: `include/hwcodec_core/decoder.hpp`

#### Lifecycle and Decoding

```cpp
bool init(const DecoderConfig& config);
bool decode_to_bgr(const EncodedPacket& packet, cv::Mat& bgr_out);
bool decode_to_jpeg(const EncodedPacket& packet, std::vector<uint8_t>& jpeg_out);
```

**Decoding Path**:
1. Primary: MPP hardware decode + RGA color conversion
2. Fallback: FFmpeg software HEVC decode (automatic)
3. Optional: JPEG encoding on success

**Input Requirements**:
- Valid, parseable HEVC bitstream in `packet.payload`
- Annex-B format with complete parameter sets for initial frames

---

## 6. Usage Examples

### 6.1 Encoding (JPEG → H.265)

```cpp
#include "hwcodec_core/encoder.hpp"

hwcodec_core::EncoderConfig cfg;
cfg.width = 1920;
cfg.height = 1080;
cfg.fps = 25;

hwcodec_core::Encoder enc;
if (!enc.init(cfg)) {
  std::cerr << "Encoder init failed" << std::endl;
  return;
}

std::vector<uint8_t> jpeg = read_jpeg_file("input.jpg");
hwcodec_core::EncodedPacket out;

if (enc.encode_jpeg(jpeg.data(), jpeg.size(), out)) {
  std::cout << "Encoded " << out.payload.size() << " bytes, keyframe: " 
            << out.is_keyframe << std::endl;
}
```

### 6.2 Decoding (H.265 → JPEG)

```cpp
#include "hwcodec_core/decoder.hpp"

hwcodec_core::DecoderConfig cfg;
cfg.width = 1920;
cfg.height = 1080;
cfg.jpeg_quality = 80;

hwcodec_core::Decoder dec;
if (!dec.init(cfg)) {
  std::cerr << "Decoder init failed" << std::endl;
  return;
}

std::vector<uint8_t> h265 = read_h265_file("input.h265");
hwcodec_core::EncodedPacket pkt;
pkt.payload = h265;

std::vector<uint8_t> jpeg_out;
if (dec.decode_to_jpeg(pkt, jpeg_out)) {
  write_jpeg_file("output.jpg", jpeg_out);
}
```

### 6.3 Tuning Recommendations

**JPEG Input Stability**:
- `jpeg_mpp_get_retry`: 8–20
- `jpeg_mpp_output_timeout_ms`: 0 (non-blocking) or 10–50 (short timeout)

**Decode Robustness**:
- `mpp_get_retry`: 8–16 (not too small)
- `mpp_output_timeout_ms`: 10–50

---

## 7. Command-Line Tools

### 7.1 `hw_encode_jpeg`

Encode JPEG image to H.265 file.

```bash
./build/hw_encode_jpeg \
  --input input.jpg \
  --output output.h265 \
  --width 1920 \
  --height 1080 \
  --fps 25
```

**Options**:
- `--width`, `--height`: Output resolution (default: 1920×1080)
- `--fps`: Frame rate (default: 25)
- `--qp-min`, `--qp-max`: QP range in CBR mode (default: 10/48)
- `--prefer-mpp-jpeg`: Prefer MPP JPEG decoder (0/1, default: 1)
- `--debug`: Enable debug logging (0/1, default: 0)

### 7.2 `hw_decode_h265`

Decode H.265 file to JPEG image.

```bash
./build/hw_decode_h265 \
  --input input.h265 \
  --output output.jpg \
  --width 1920 \
  --height 1080 \
  --quality 80
```

**Options**:
- `--width`, `--height`: Source resolution (default: 1920×1080)
- `--quality`: JPEG quality (default: 80)
- `--mpp-split-mode`: MPP split mode (default: 1)
- `--mpp-timeout-ms`: MPP frame retrieval timeout (default: 0)
- `--debug`: Enable debug logging

### 7.3 `ffmpeg_rkmpp_probe`

Verify FFmpeg `mjpeg_rkmpp` and `hevc_rkmpp` functionality with timing reports.

```bash
./build/ffmpeg_rkmpp_probe \
  --input input.jpg \
  --output output.h265 \
  --width 1920 \
  --height 1080 \
  --fps 25
```

---

## 8. Processing Pipeline

### 8.1 Encoding Path

1. Accept BGR or JPEG input
2. If JPEG: attempt MPP hardware decode → fallback to OpenCV
3. RGA color conversion/scaling → NV12 preparation
4. FFmpeg `hevc_rkmpp` H.265 encoding
5. Output `EncodedPacket`; call `flush()` for delayed packets

### 8.2 Decoding Path

1. Receive H.265 bitstream in `EncodedPacket`
2. MPP push frame + retrieve output
3. On hardware success: RGA NV12 → RGB → BGR (OpenCV)
4. On failure: FFmpeg HEVC software decode → BGR
5. Optional: encode to JPEG

---

## 9. Recent Fixes and Compatibility

**Key improvements in current version**:

1. **RGA Color Conversion**: `imcopy` → `imcvtcolor` (correct cross-format handling)
2. **MPP Split Mode Timing**: `MPP_DEC_SET_PARSER_SPLIT_MODE` before `mpp_init`
3. **Decode Robustness**: Frame retrieval polling with retry logic
4. **BGR Stride Handling**: Explicit wstride/hstride to RGA wrapper
5. **RPATH Configuration**: Moved from static libraries to executables

---

## 10. ROS Integration Guide

### Recommended Pattern

1. ROS nodes handle subscribe/publish only
2. All codec logic in `libhwcodec_core`
3. Fixed-version dependencies (FFmpeg, MPP, RGA)
4. Avoid global `LD_LIBRARY_PATH` reliance

### Parameter Mapping

- **Encoder Config** → `EncoderConfig.width/height/fps/codec_name/bitrate/gop/profile`
- **Decoder Config** → `DecoderConfig.width/height/jpeg_quality/mpp_*`
- See [ROS integration notes](codec_acc.yaml) for detailed mappings

---

## 11. Troubleshooting (FAQ)

### Q: Encoder initialization fails

**Check**:
- System FFmpeg includes `hevc_rkmpp` codec
- `codec_name` is correct
- Rockchip libraries are accessible at runtime

### Q: Decoder output is missing or intermittent

**Check**:
- Input bitstream is complete and valid
- Split mode is correct for fragmented inputs
- No error/discard frames in logs

### Q: Output colors are wrong

**Check**:
- Input pixel format matches expectations
- RGA color conversion uses `imcvtcolor`
- Width/height/stride parameters match input

### Q: Runtime library not found

**Check**:
- Target library paths are correct
- Executable RPATH includes dependency directories
- Deployment directories match build configuration

---

## 12. Maintenance Guidelines

After upgrading `librga`, `MPP`, or `ffmpeg-rockchip`:

1. Run smoke tests:
   - JPEG → H.265
   - H.265 → JPEG
   - BGR with non-contiguous stride input

2. Add platform-specific compilation and execution tests to CI

---

## 13. Performance Notes

- **Real-time Streaming**: Use synchronous API, accept occasional frame skips
- **Batch Processing**: Request all frames first, then `flush()` once for delayed packets
- **Cross-Process Communication**: Use stable `EncodedPacket` contract

---

### License

[Specify your license here]

### Contributing

Contributions are welcome. Please ensure:
- Code follows project style guidelines
- Changes include relevant test coverage
- Commits reference GitHub issues where applicable

### Support

For issues or questions:
- Review the [Troubleshooting (FAQ)](#11-troubleshooting-faq) section
- Open an issue with platform details and relevant logs

---

<a name="中文"></a>

## 中文

### 概述

**hwcodec_core** 是一个为Rockchip平台（RK3588及兼容芯片）设计的硬件加速编解码库。它提供高性能H.265编解码支持和JPEG处理，通过原生硬件加速实现，并具有自动软件降级功能。

#### 主要特性

- **H.265编码**: BGR/JPEG → H.265 (FFmpeg Rockchip编码器)
- **H.265解码**: H.265 → BGR/JPEG (MPP + RGA)
- **硬件加速**: 充分利用Rockchip MPP和RGA
- **优雅降级**: 硬件失败时自动切换到FFmpeg软件编解码
- **独立CMake编译**: 不依赖ROS
- **生产就绪**: 已在ROS集成中使用，拥有稳定的接口契约

#### 应用场景

- 实时视频编解码的嵌入式视觉系统
- 视频预处理的边缘AI推理
- Rockchip开发板上的硬件加速视频流
- 批量视频处理流水线

---

### 1. 文档说明

#### 1.1 文档目的

本文档用于说明`hwcodec_core`硬件编解码核心模块的设计、目录结构、构建方式、接口使用、CLI工具、部署建议与常见问题。

#### 1.2 适用范围

当前版本面向Rockchip平台下的H.265编解码场景，包含：

- **H.265编码**: `BGR/JPEG → H.265`
- **H.265解码**: `H.265 → BGR/JPEG`
- **独立CMake构建**: 无ROS依赖

---

### 2. 模块概览

#### 2.1 设计目标

将"硬件编解码业务逻辑"从ROS节点中解耦，形成可独立复用的核心库。

**收益**:

- ROS节点更轻量，仅负责消息收发
- 编解码逻辑可独立测试和部署
- 依赖管理更集中，降低系统耦合

#### 2.2 主要能力

##### `hwcodec_core::Encoder`
- **输入**: BGR原始图像或JPEG字节流
- **输出**: H.265码流包(`EncodedPacket`)

##### `hwcodec_core::Decoder`
- **输入**: H.265码流包(`EncodedPacket`)
- **输出**: BGR图像或JPEG字节流

---

### 3. 项目目录结构

```text
hwcodec_core/
  CMakeLists.txt
  README.md
  build.sh
  apps/
    hw_encode_jpeg.cpp
    hw_decode_h265.cpp
    ffmpeg_rkmpp_probe.cpp
  include/
    hwcodec_core/
      types.hpp
      encoder.hpp
      decoder.hpp
      internal/
        rkmpp_decoder.hpp
        rga_converter.hpp
  src/
    encoder.cpp
    decoder.cpp
    rkmpp_decoder.cpp
    rga_converter.cpp
  cmake/
    (CMake模块)
  assets/
    (资源文件)
```

---

### 4. 构建与安装

#### 4.1 依赖要求

- CMake >= 3.16
- C++14编译器
- OpenCV
- FFmpeg(需包含Rockchip编码器`hevc_rkmpp`)
- Rockchip MPP(`rockchip_mpp.pc`)
- Rockchip RGA(`librga.so`)

#### 4.2 编译命令

```bash
cd hwcodec_core
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

#### 4.3 安装命令(可选)

```bash
cmake --install build
```

---

### 5. API参考

#### 5.1 数据类型

**头文件**: `include/hwcodec_core/types.hpp`

```cpp
struct EncodedPacket {
  uint64_t stamp_ns = 0;        // 可选时间戳(纳秒)
  bool is_keyframe = false;      // 关键帧标记(IDR/CRA)
  std::vector<uint8_t> payload;  // HEVC码流字节
};
```

**payload格式**:
- 推荐格式: Annex-B(`00 00 01`或`00 00 00 01`起始码)
- 关键帧应携带VPS/SPS/PPS，确保下游解码器可恢复解码

#### 5.2 编码API

**头文件**: `include/hwcodec_core/encoder.hpp`

##### 生命周期

```cpp
bool init(const EncoderConfig& config);
bool encode_bgr(const uint8_t* bgr_data, int width, int height, 
                int stride_bytes, EncodedPacket& out_packet);
bool encode_jpeg(const uint8_t* jpeg_data, size_t jpeg_size, 
                 EncodedPacket& out_packet);
```

**线程安全**: 单个实例应由单线程驱动；并发编码流需使用多个实例

**参数说明**:
- `bgr_data/jpeg_data`: 输入缓冲区首地址
- `stride_bytes`: BGR行步长(字节)，必须`>= width * 3`
- `out_packet`: 输出参数，成功时填充payload/is_keyframe/stamp_ns

**返回值**: 成功返回`true`，失败返回`false`

**重要提示**:
- `false`不总是致命错误；实时编码场景可能因内部缓冲而暂未产包
- 不要在实时流中每帧调用`flush()`；使用异步流水线以优化吞吐

##### 异步流水线接口

```cpp
bool submit_bgr(...);
bool submit_jpeg(...);
bool receive_packet(EncodedPacket& out_packet);
bool flush(EncodedPacket& out_packet);
```

#### 5.3 解码API

**头文件**: `include/hwcodec_core/decoder.hpp`

##### 生命周期与解码

```cpp
bool init(const DecoderConfig& config);
bool decode_to_bgr(const EncodedPacket& packet, cv::Mat& bgr_out);
bool decode_to_jpeg(const EncodedPacket& packet, std::vector<uint8_t>& jpeg_out);
```

**解码路径**:
1. 优先: MPP硬解 + RGA色彩转换
2. 降级: FFmpeg软解HEVC(自动)
3. 可选: 编码为JPEG输出

**输入要求**:
- `packet.payload`中的HEVC码流必须完整可解析
- 推荐Annex-B格式，起始帧必须包含完整参数集

---

### 6. 使用示例

#### 6.1 编码示例(JPEG → H.265)

```cpp
#include "hwcodec_core/encoder.hpp"

hwcodec_core::EncoderConfig cfg;
cfg.width = 1920;
cfg.height = 1080;
cfg.fps = 25;

hwcodec_core::Encoder enc;
if (!enc.init(cfg)) {
  std::cerr << "编码器初始化失败" << std::endl;
  return;
}

std::vector<uint8_t> jpeg = read_jpeg_file("input.jpg");
hwcodec_core::EncodedPacket out;

if (enc.encode_jpeg(jpeg.data(), jpeg.size(), out)) {
  std::cout << "编码" << out.payload.size() << "字节, 关键帧: " 
            << out.is_keyframe << std::endl;
}
```

#### 6.2 解码示例(H.265 → JPEG)

```cpp
#include "hwcodec_core/decoder.hpp"

hwcodec_core::DecoderConfig cfg;
cfg.width = 1920;
cfg.height = 1080;
cfg.jpeg_quality = 80;

hwcodec_core::Decoder dec;
if (!dec.init(cfg)) {
  std::cerr << "解码器初始化失败" << std::endl;
  return;
}

std::vector<uint8_t> h265 = read_h265_file("input.h265");
hwcodec_core::EncodedPacket pkt;
pkt.payload = h265;

std::vector<uint8_t> jpeg_out;
if (dec.decode_to_jpeg(pkt, jpeg_out)) {
  write_jpeg_file("output.jpg", jpeg_out);
}
```

#### 6.3 常用调参建议

**JPEG输入稳定性**:
- `jpeg_mpp_get_retry`: 建议8~20
- `jpeg_mpp_output_timeout_ms`: 建议0(非阻塞)或10~50(短超时)

**解码硬解稳定性**:
- `mpp_get_retry`: 建议8~16(不过小)
- `mpp_output_timeout_ms`: 建议10~50

---

### 7. CLI工具说明

#### 7.1 `hw_encode_jpeg`

读取JPEG文件并编码为H.265。

```bash
./build/hw_encode_jpeg \
  --input input.jpg \
  --output output.h265 \
  --width 1920 \
  --height 1080 \
  --fps 25
```

**参数说明**:
- `--width`, `--height`: 输出分辨率(默认1920×1080)
- `--fps`: 帧率(默认25)
- `--qp-min`, `--qp-max`: CBR模式下QP范围(默认10/48)
- `--prefer-mpp-jpeg`: 是否优先MPP JPEG解码(0/1，默认1)
- `--debug`: 调试日志(0/1，默认0)

#### 7.2 `hw_decode_h265`

读取H.265文件并解码为JPEG。

```bash
./build/hw_decode_h265 \
  --input input.h265 \
  --output output.jpg \
  --width 1920 \
  --height 1080 \
  --quality 80
```

**参数说明**:
- `--width`, `--height`: 源分辨率(默认1920×1080)
- `--quality`: JPEG质量(默认80)
- `--mpp-split-mode`: MPP split模式(默认1)
- `--mpp-timeout-ms`: MPP取帧超时(默认0)
- `--debug`: 调试日志开关

#### 7.3 `ffmpeg_rkmpp_probe`

对照验证FFmpeg的`mjpeg_rkmpp`硬解和`hevc_rkmpp`硬编码能力。

```bash
./build/ffmpeg_rkmpp_probe \
  --input input.jpg \
  --output output.h265 \
  --width 1920 \
  --height 1080 \
  --fps 25
```

---

### 8. 运行机制说明

#### 8.1 编码路径

1. 输入BGR或JPEG
2. 若为JPEG，优先走MPP硬件JPEG解码；失败时回退OpenCV解码
3. 通过RGA做色彩转换/缩放并准备NV12
4. 调用FFmpeg `hevc_rkmpp`编码
5. 输出`EncodedPacket`(必要时可调用`flush`获取延迟输出包)

#### 8.2 解码路径

1. 输入H.265码流包
2. 调用MPP送包并取帧
3. MPP硬解成功时，通过RGA做NV12 → RGB，再转BGR(OpenCV)
4. 若MPP或RGA失败，自动回退FFmpeg软解(HEVC)并转换为BGR
5. 可选编码为JPEG输出

---

### 9. 近期修复与兼容性要点

当前版本已完成以下关键修复：

1. **RGA色彩转换接口修复**: `imcopy` 改为 `imcvtcolor`，避免跨格式转换误用
2. **MPP split模式设置时序修复**: `MPP_DEC_SET_PARSER_SPLIT_MODE`调整到`mpp_init`前
3. **解码取帧鲁棒性增强**: 增加`decode_get_frame`轮询重试机制
4. **BGR stride传递修复**: RGA wrapper显式传入`wstride/hstride`
5. **RPATH生效目标修复**: RPATH配置从静态库转移到可执行目标

---

### 10. 与ROS集成建议

推荐模式：

1. ROS节点只负责订阅/发布
2. 编解码逻辑全部下沉到`libhwcodec_core`
3. 通过固定版本依赖目录部署FFmpeg/MPP/RGA
4. 避免过度依赖全局`LD_LIBRARY_PATH`

#### 参数映射

- **编码器配置** → `EncoderConfig.width/height/fps/codec_name/bitrate/gop/profile`
- **解码器配置** → `DecoderConfig.width/height/jpeg_quality/mpp_*`

---

### 11. 常见问题(FAQ)

#### Q: 编码器初始化失败

**优先检查**:
- 系统FFmpeg是否包含`hevc_rkmpp`
- `codec_name`是否正确
- 运行时是否能找到Rockchip相关动态库

#### Q: 解码偶发失败或无输出

**优先检查**:
- 输入码流是否完整
- 分片码流场景下split模式是否有效
- 是否存在异常帧(被errinfo/discard丢弃)

#### Q: 输出颜色异常

**优先检查**:
- 输入像素格式是否符合预期
- RGA色彩转换路径是否走`imcvtcolor`
- 宽高与stride参数是否匹配

#### Q: 运行时报库找不到

**优先检查**:
- 目标机库路径是否正确
- 可执行文件RPATH配置是否包含依赖目录
- 实际安装目录是否和部署脚本一致

---

### 12. 版本维护建议

每次升级`librga`/`MPP`/`ffmpeg-rockchip`后，执行最小冒烟回归：

1. JPEG → H.265
2. H.265 → JPEG
3. BGR非紧凑stride输入

在CI中增加目标平台编译和运行测试。

---

### 13. 性能说明

- **实时流**: 优先同步接口，允许偶发帧跳
- **批处理**: 完成全部输入后调用一次`flush()`拉取尾包
- **跨进程通信**: 使用稳定的`EncodedPacket`契约

---

### License

[请指定您的许可证]

### Contributing

欢迎贡献。请确保：
- 代码遵循项目风格指南
- 变更包括相关测试
- 提交信息引用相关GitHub issue

---

**Last Updated**: 2026-04-18  
**Compatible Boards**: RK3588 and compatible Rockchip SoCs  
**Build System**: CMake 3.16+  
**Language**: C++14
