# hwcodec_core 用户开发手册

## 1. 文档说明

### 1.1 文档目的

本文档用于说明 `hwcodec_core` 硬件编解码核心模块的设计、目录结构、构建方式、接口使用、CLI工具、部署建议与常见问题，帮助开发和联调人员快速接入。

### 1.2 适用范围

当前版本面向 Rockchip 平台下的 H.265 编解码场景，包含：

- H.265 编码：`BGR/JPEG -> H.265`（FFmpeg Rockchip 编码器）
- H.265 解码：`H.265 -> BGR/JPEG`（MPP + RGA）
- 独立 CMake 构建（无 ROS 依赖）

## 2. 模块概览

### 2.1 设计目标

`hwcodec_core` 的目标是将“硬件编解码业务逻辑”从 ROS 节点中解耦，形成可独立复用的核心库。

收益：

- ROS 节点更轻量，仅负责消息收发
- 编解码逻辑可独立测试和部署
- 依赖管理更集中，降低系统耦合

### 2.2 主要能力

- `hwcodec_core::Encoder`
  - 输入：BGR 原始图像或 JPEG 字节流
  - 输出：H.265 码流包（`EncodedPacket`）
- `hwcodec_core::Decoder`
  - 输入：H.265 码流包（`EncodedPacket`）
  - 输出：BGR 图像或 JPEG 字节流

## 3. 项目目录结构

```text
hwcodec_core/
  CMakeLists.txt
  README.md
  README_CN.md
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
```

## 4. 构建与安装

### 4.1 依赖要求

- CMake >= 3.16
- C++14 编译器
- OpenCV
- FFmpeg（需包含 Rockchip 编码器 `hevc_rkmpp`）
- Rockchip MPP（`rockchip_mpp.pc`）
- Rockchip RGA（`librga.so`）

### 4.2 编译命令

```bash
cd hwcodec_core
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

### 4.3 安装命令（可选）

```bash
cmake --install build
```

## 5. API 参考（Google 风格）

本节采用“概览 -> 签名 -> 参数 -> 返回值 -> 失败语义 -> 线程安全”的结构，便于快速查阅与联调。

### 5.1 数据契约

头文件：`include/hwcodec_core/types.hpp`

```cpp
struct EncodedPacket {
  uint64_t stamp_ns = 0;
  bool is_keyframe = false;
  std::vector<uint8_t> payload;
};
```

字段定义：

- `stamp_ns`：可选时间戳（纳秒）。
- `is_keyframe`：关键帧标记（IDR/CRA）。
- `payload`：HEVC 码流字节。
  - 推荐格式：Annex-B（`00 00 01` 或 `00 00 00 01` 起始码）。
  - 建议关键帧携带 VPS/SPS/PPS，确保任意时刻下游可恢复解码。

### 5.2 编码 API：`hwcodec_core::Encoder`

头文件：`include/hwcodec_core/encoder.hpp`

#### 5.2.1 生命周期

签名：

- `Encoder();`
- `~Encoder();`
- `bool init(const EncoderConfig& config);`

前置条件：

- `init()` 必须在任何编码调用前执行一次。
- `config.width/height/fps` 必须为正数。

返回值与失败语义：

- `true`：初始化成功，可进入编码阶段。
- `false`：初始化失败，常见原因为编码器不可用、参数非法、依赖库不可达。

线程安全：

- `Encoder` 实例不是可重入对象；同一实例应由单线程驱动调用接口。

#### 5.2.2 同步编码接口

签名：

- `bool encode_bgr(const uint8_t* bgr_data, int width, int height, int stride_bytes, EncodedPacket& out_packet);`
- `bool encode_jpeg(const uint8_t* jpeg_data, size_t jpeg_size, EncodedPacket& out_packet);`

参数说明：

- `bgr_data/jpeg_data`：输入缓冲区首地址，调用期间必须有效。
- `stride_bytes`：BGR 行步长（字节），必须 `>= width * 3`。
- `out_packet`：输出参数，成功时填充 `payload/is_keyframe/stamp_ns`。

返回值与失败语义：

- `true`：本次调用产出有效包。
- `false`：本次调用未产包或失败（输入非法、内部缓冲、编码失败等）。

注意事项：

- `false` 不总是致命错误，实时编码场景可能因内部缓冲而暂未产包。
- 不要在实时流中每帧调用 `flush()`，`flush()` 语义是发送 EOS 并进入 drain。

#### 5.2.3 异步流水线接口

签名：

- `bool submit_bgr(...);`
- `bool submit_jpeg(...);`
- `bool receive_packet(EncodedPacket& out_packet);`

行为说明：

- 先 `submit_*`，后 `receive_packet()`，结果按 FIFO 顺序返回。
- 适用于吞吐优先场景（批量送入，多次取包）。

#### 5.2.4 `flush()` 接口语义

签名：

- `bool flush(EncodedPacket& out_packet);`

语义：

- 触发编码器 drain，尝试拉取延迟输出包。

使用场景：

- 文件结束、会话结束、离线批处理收尾。

不推荐场景：

- 实时连续视频流的逐帧主路径。

#### 5.2.5 `EncoderConfig` 关键字段

- `codec_name`：默认 `hevc_rkmpp`。
- `bitrate/gop/bf/rc_mode/profile/qp_min/qp_max`：码率与质量控制。
- `prefer_mpp_jpeg_decoder/jpeg_mpp_*`：JPEG 硬解路径调优。
- `debug`：详细日志开关（亦可通过环境变量 `HWCODEC_DEBUG=1` 打开）。

### 5.3 解码 API：`hwcodec_core::Decoder`

头文件：`include/hwcodec_core/decoder.hpp`

#### 5.3.1 生命周期

签名：

- `Decoder();`
- `~Decoder();`
- `bool init(const DecoderConfig& config);`

前置条件：

- `init()` 必须先于任何 `decode_*` 调用。

返回值与失败语义：

- `true`：初始化成功。
- `false`：初始化失败（参数非法、MPP/RGA/软解初始化失败）。

#### 5.3.2 解码接口

签名：

- `bool decode_to_bgr(const EncodedPacket& packet, cv::Mat& bgr_out);`
- `bool decode_to_jpeg(const EncodedPacket& packet, std::vector<uint8_t>& jpeg_out);`

输入要求：

- `packet.payload` 必须是可解析的 HEVC 码流包。
- 推荐输入为 Annex-B，并保证关键参数集可获取。

行为说明：

- 首选 MPP 硬解 + RGA 转换。
- 硬解路径失败时自动回退 FFmpeg 软解 HEVC。
- `decode_to_jpeg()` 内部流程为 `decode_to_bgr()` 后再 JPEG 编码。

#### 5.3.3 `DecoderConfig` 关键字段

- `mpp_split_mode`：H.265 parser split 模式。
- `mpp_output_timeout_ms`：`0` 非阻塞，`<0` 阻塞，`>0` 超时毫秒。
- `mpp_put_retry/mpp_get_retry`：送包与取帧重试次数。
- `debug`：详细日志开关。

### 5.4 API 使用建议

- 实时在线流：优先同步接口，允许偶发“本帧未产包”，不要逐帧 `flush()`。
- 离线批处理：完成全部输入后调用一次 `flush()` 拉取尾包。
- 跨进程/跨节点传输：固定 `EncodedPacket` 契约，避免自定义二次封装。

## 6. 使用示例

### 6.1 编码示例（JPEG -> H.265）

```cpp
hwcodec_core::EncoderConfig cfg;
cfg.width = 1920;
cfg.height = 1080;
cfg.fps = 25;

hwcodec_core::Encoder enc;
if (!enc.init(cfg)) {
  return;
}

hwcodec_core::EncodedPacket out;
if (enc.encode_jpeg(jpeg.data(), jpeg.size(), out)) {
  // out.payload 即 H.265 数据
}
```

### 6.2 解码示例（H.265 -> JPEG）

```cpp
hwcodec_core::DecoderConfig cfg;
cfg.width = 1920;
cfg.height = 1080;
cfg.jpeg_quality = 80;

hwcodec_core::Decoder dec;
if (!dec.init(cfg)) {
  return;
}

hwcodec_core::EncodedPacket in;
in.payload = h265_bytes;

std::vector<uint8_t> jpeg;
if (dec.decode_to_jpeg(in, jpeg)) {
  // jpeg 即解码后 JPEG 数据
}
```

### 6.3 常用调参建议

- 编码 JPEG 输入稳定性：
  - `jpeg_mpp_get_retry` 建议 `8~20`
  - `jpeg_mpp_output_timeout_ms` 建议 `0`（非阻塞）或 `10~50`（短超时）
- 解码硬解稳定性：
  - `mpp_get_retry` 不建议过小，建议 `8~16`
  - `mpp_output_timeout_ms` 建议 `10~50`
- 若业务优先“必达”而非“纯硬件路径”，保持默认软解回退开启（当前实现已默认开启）。

## 7. CLI 工具说明

### 7.1 `hw_encode_jpeg`

用途：读取 JPEG 文件并编码为 H.265。

命令格式：

```bash
./build/hw_encode_jpeg --input in.jpg --output out.h265 --width 1920 --height 1080 --fps 25
```

参数说明：

- `--input`：输入 JPEG 文件
- `--output`：输出 H.265 文件
- `--width`：目标宽度（可选，默认 1920）
- `--height`：目标高度（可选，默认 1080）
- `--fps`：帧率（可选，默认 25）
- `--debug`：调试日志（可选，`0/1`，默认 `0`）
- `--qp-min` / `--qp-max`：CBR模式下QP范围（可选，默认 `10/48`）
- `--prefer-mpp-jpeg`：是否优先MPP JPEG解码（可选，`0/1`，默认 `1`）
- `--jpeg-mpp-timeout-ms`：MPP JPEG取帧超时（可选，默认 `0`）
- `--jpeg-mpp-put-retry` / `--jpeg-mpp-get-retry`：送包/取帧重试次数（可选，默认 `5/20`）
- `--jpeg-mpp-put-sleep-ms` / `--jpeg-mpp-get-sleep-ms`：重试间隔毫秒（可选，默认 `1/1`）
- `--jpeg-mpp-eos`：送包时是否设置EOS（可选，`0/1`，默认 `0`）

### 7.2 `hw_decode_h265`

用途：读取 H.265 文件并解码为 JPEG。

命令格式：

```bash
./build/hw_decode_h265 --input in.h265 --output out.jpg --width 1920 --height 1080 --quality 80
```

参数说明：

- `--input`：输入 H.265 文件
- `--output`：输出 JPEG 文件
- `--width`：目标宽度（可选，默认 1920）
- `--height`：目标高度（可选，默认 1080）
- `--quality`：JPEG质量（可选，默认 80）
- `--mpp-split-mode`：MPP split 模式（可选，默认 `1`）
- `--mpp-timeout-ms`：MPP取帧超时（可选，默认 `0`）
- `--mpp-put-retry` / `--mpp-get-retry`：送包/取帧重试次数（可选，默认 `5/15`）
- `--mpp-put-sleep-ms` / `--mpp-get-sleep-ms`：重试间隔毫秒（可选，默认 `2/1`）
- `--debug`：调试日志开关（可选，支持 `--debug` 或 `--debug 1`，默认 `0`）

### 7.3 `ffmpeg_rkmpp_probe`

用途：对照验证 FFmpeg `mjpeg_rkmpp` 硬解和 `hevc_rkmpp` 硬编码能力，并输出每步耗时。

命令格式：

```bash
./build/ffmpeg_rkmpp_probe --input in.jpg --output out.h265 --width 1920 --height 1080 --fps 25
```

参数说明：

- `--input`：输入 JPEG 文件
- `--output`：输出 H.265 文件
- `--ffmpeg`：ffmpeg 可执行程序路径（可选，默认 `ffmpeg`）
- `--width`：目标宽度（可选，默认 `1920`）
- `--height`：目标高度（可选，默认 `1080`）
- `--fps`：目标帧率（可选，默认 `25`）
- `--verbose`：打印实际执行命令（可选，`0/1`，默认 `0`）

## 8. 运行机制说明

### 8.1 编码路径

1. 输入 BGR 或 JPEG。
2. 若为 JPEG，优先走 MPP 硬件 JPEG 解码；失败时回退 OpenCV 解码。
3. 通过 RGA 做色彩转换/缩放并准备 NV12。
4. 调用 FFmpeg `hevc_rkmpp` 编码。
5. 输出 `EncodedPacket`（必要时可调用 `flush` 获取延迟输出包）。

### 8.2 解码路径

1. 输入 H.265 码流包。
2. 调用 MPP 送包并取帧。
3. MPP硬解成功时，通过 RGA 做 NV12 -> RGB，再转 BGR（OpenCV）。
4. 若 MPP 或 RGA 失败，自动回退 FFmpeg 软解（HEVC）并转换为 BGR。
5. 可选编码为 JPEG 输出。

## 9. 近期修复与兼容性要点

当前版本已完成以下关键修复：

1. RGA 色彩转换接口修复
   - 由 `imcopy` 改为 `imcvtcolor`，避免跨格式转换误用。
2. MPP split 模式设置时序修复
   - `MPP_DEC_SET_PARSER_SPLIT_MODE` 调整到 `mpp_init` 前。
3. 解码取帧鲁棒性增强
   - 增加 `decode_get_frame` 轮询重试机制。
4. BGR stride 传递修复
   - RGA wrapper 显式传入 `wstride/hstride`。
5. RPATH 生效目标修复
   - RPATH 配置从静态库转移到可执行目标。

## 10. 与 ROS 集成建议

推荐模式：

1. ROS 节点只负责订阅/发布。
2. 编解码逻辑全部下沉到 `libhwcodec_core`。
3. 通过固定版本依赖目录部署 FFmpeg/MPP/RGA。
4. 避免过度依赖全局 `LD_LIBRARY_PATH`。

### 10.1 `src_hw/codec_acc` 参数映射

`codec_acc` 作为薄壳，建议直接透传 `hwcodec_core` 配置。当前参数映射如下：

- 编码节点 `codec_acc_encode_bridge`
  - `encode/source/*` -> `EncoderConfig.width/height/fps` 与输入模式
  - `encode/encoder/*` -> `EncoderConfig.codec_name/bitrate/gop/bf/rc_mode/profile/qp_min/qp_max/debug`
  - `encode/jpeg_decoder/*` -> `EncoderConfig.prefer_mpp_jpeg_decoder/jpeg_mpp_*`
- 解码节点 `codec_acc_decode_bridge`
  - `decode/source/width|height` -> `DecoderConfig.width|height`
  - `decode/jpeg_output/quality` -> `DecoderConfig.jpeg_quality`
  - `decode/decoder/*` -> `DecoderConfig.mpp_*` 与 `debug`

推荐先从 `src_hw/codec_acc/config/codec_acc.yaml` 默认值起步，再按平台压测调优。

## 11. 常见问题（FAQ）

### 11.1 编码器初始化失败

优先检查：

- 系统 FFmpeg 是否包含 `hevc_rkmpp`
- `codec_name` 是否正确
- 运行时是否能找到 Rockchip 相关动态库

### 11.2 解码偶发失败或无输出

优先检查：

- 输入码流是否完整
- 分片码流场景下 split 模式是否有效
- 是否存在异常帧（被 `errinfo/discard` 丢弃）

### 11.3 输出颜色异常

优先检查：

- 输入像素格式是否符合预期
- RGA 色彩转换路径是否走 `imcvtcolor`
- 宽高与stride参数是否匹配

### 11.4 运行时报库找不到

优先检查：

- 目标机库路径是否正确
- 可执行文件 RPATH 配置是否包含依赖目录
- 实际安装目录是否和部署脚本一致

## 12. 版本维护建议

- 每次升级 `librga` / `MPP` / `ffmpeg-rockchip` 后，执行一次最小冒烟回归：
  - JPEG -> H.265
  - H.265 -> JPEG
  - BGR 非紧凑 stride 输入
- 建议在 CI 中增加目标平台编译和运行测试。
