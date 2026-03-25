# hwcodec_core Developer Guide

Document status: Draft for internal integration

Last updated: 2026-03-25

## 1. Purpose

`hwcodec_core` is a standalone hardware codec core extracted from ROS logic.

It provides:
- `hwcodec_core::Encoder`: `BGR/JPEG -> H.265 (hevc_rkmpp)`
- `hwcodec_core::Decoder`: `H.265 -> BGR/JPEG (MPP + RGA)`
- CLI tools:
  - `hw_encode_jpeg`
  - `hw_decode_h265`

This guide includes:
- code compliance review against `librga`, `MPP`, `ffmpeg-rockchip` developer docs
- API/signature conflict analysis
- project structure and usage manual

## 2. Scope And Review Method

### 2.1 Reviewed source scope

- `hwcodec_core/include/**`
- `hwcodec_core/src/**`
- `hwcodec_core/apps/**`
- `hwcodec_core/CMakeLists.txt`

### 2.2 Compared references

- RGA docs and headers:
  - `librga-main/docs/Rockchip_Developer_Guide_RGA_EN.md`
  - `librga-main/include/im2d_single.h`
  - `librga-main/include/im2d_buffer.h`
- MPP docs and headers:
  - `mpp-develop/doc/Rockchip_Developer_Guide_MPP_EN.md`
  - `mpp-develop/inc/rk_mpi.h`
  - `mpp-develop/inc/rk_mpi_cmd.h`
  - `mpp-develop/inc/mpp_packet.h`
- FFmpeg Rockchip implementation:
  - `ffmpeg-rockchip-master/libavcodec/rkmppenc.h`
  - `ffmpeg-rockchip-master/libavcodec/rkmppenc.c`

### 2.3 Validation limitations

- This environment has no `cmake` executable available; compile-time verification was not runnable.
- Findings are from static review and upstream API/doc comparison.

## 3. Executive Summary

Current `hwcodec_core` has **no obvious compile-time function signature mismatch** in main API calls, but has **multiple high-risk semantic API misuse issues** that can cause runtime failure or unstable behavior.

Overall status: **Not production-ready** before fixing the issues in section 4.

## 3.1 Fix Status (2026-03-25)

Issues 1-5 from the previous review have been fixed in code:
- fixed color conversion API usage (`imcopy` -> `imcvtcolor`)
- fixed MPP split-mode setup order (now before `mpp_init`)
- added bounded `decode_get_frame` polling/retry flow
- fixed BGR stride propagation in RGA buffer wrapping
- moved RPATH settings from static library target to executable targets

## 4. Compliance Findings (Priority Ordered)

### 4.1 Critical: RGA API misuse for color conversion

- File: `hwcodec_core/src/encoder.cpp:152-160`
- File: `hwcodec_core/src/rga_converter.cpp:22-26`

Current code uses `imcopy(src, dst)` for format conversion (`BGR <-> NV12`, `NV12 -> RGB`).

According to RGA docs:
- `imcopy` is for image copy.
- `imcvtcolor` is for image format conversion.

References:
- `librga-main/docs/Rockchip_Developer_Guide_RGA_EN.md:767,776,1575,1801`
- `librga-main/include/im2d_single.h:39,127`

Impact:
- conversion may fail or behave unpredictably on different RGA/SoC versions.

Recommendation:
- replace these conversion paths with `imcvtcolor(src, dst, src.format, dst.format)`.

### 4.2 High: MPP split parser control timing violates official requirement

- File: `hwcodec_core/src/rkmpp_decoder.cpp:47-55`

Current sequence:
1. `mpp_create`
2. `mpp_init`
3. `MPP_DEC_SET_PARSER_SPLIT_MODE`

MPP doc explicitly requires `MPP_DEC_SET_PARSER_SPLIT_MODE` **before `mpp_init`**.

References:
- `mpp-develop/doc/Rockchip_Developer_Guide_MPP_EN.md:301,385`
- `mpp-develop/inc/rk_mpi_cmd.h:91`

Impact:
- split mode may not take effect consistently.

Recommendation:
- set split mode after `mpp_create` and before `mpp_init`.

### 4.3 High: Decoder get-frame flow too optimistic

- File: `hwcodec_core/src/rkmpp_decoder.cpp:100-103`

Current logic calls `decode_get_frame` once and treats no-frame as hard failure.

MPP async model expects repeated `decode_put_packet/decode_get_frame` handling and null-frame cases.

References:
- `mpp-develop/doc/Rockchip_Developer_Guide_MPP_EN.md:261-262,283,319`
- `mpp-develop/inc/rk_mpi.h:103,112`

Impact:
- intermittent frame drops, decode false negatives under queue pressure.

Recommendation:
- add bounded polling loop for `decode_get_frame`, with proper timeout/backoff.

### 4.4 Medium: BGR stride handling is lossy in RGA wrapper call

- File: `hwcodec_core/src/encoder.cpp:109,153-156`

`encode_bgr()` accepts `stride_bytes`, but `wrapbuffer_virtualaddr(...)` call does not pass explicit stride (`wstride/hstride`) for source `cv::Mat`.

Reference:
- `librga-main/include/im2d_buffer.h:117,166`

Impact:
- wrong conversion for non-tight-packed input rows.

Recommendation:
- call stride-aware wrap form and pass correct `wstride/hstride` derived from `cv::Mat::step`.

### 4.5 Medium: RPATH configured on static library has no runtime effect

- File: `hwcodec_core/CMakeLists.txt:23,54-58`

`hwcodec_core` is `STATIC`; setting `BUILD_RPATH` there does not solve executable runtime loader search path.

Impact:
- deployment assumptions can break.

Recommendation:
- set RPATH on final executables/shared libs that are loaded at runtime.

### 4.6 Medium: Include path portability risk

- File: `hwcodec_core/include/hwcodec_core/internal/rkmpp_decoder.hpp:6-8`

Using `<rockchip/...>` is valid in many distro packages, but some builds expose flat includes (`<rk_mpi.h>` etc).

Impact:
- cross-device portability friction.

Recommendation:
- keep current include if target image guarantees `/usr/include/rockchip`, or add fallback include strategy.

## 5. Function Signature Check Result

### 5.1 Checked and consistent

- `mpp_packet_init(MppPacket*, void*, size_t)` usage is signature-correct.
- `mpp_packet_set_data`, `mpp_packet_set_size`, `mpp_packet_set_pos`, `mpp_packet_set_length` usage forms are signature-correct.
- `decode_put_packet`, `decode_get_frame` invocation signatures are correct.
- `wrapbuffer_fd`, `wrapbuffer_virtualaddr` call forms are syntactically valid.

### 5.2 Not signature errors but semantic misuse

- `imcopy` used where `imcvtcolor` should be used (section 4.1).

## 6. FFmpeg Rockchip Option Compatibility

Current encoder config:
- codec: `hevc_rkmpp`
- options: `rc_mode`, `qp_min`, `qp_max`

`ffmpeg-rockchip` defines these options for rkmpp encoder.

Reference:
- `ffmpeg-rockchip-master/libavcodec/rkmppenc.h:104-119`

Note:
- Code currently sets `rc_mode` with numeric strings (`"1"`, `"0"`).
- This is generally accepted by AVOption integer parsing, but using canonical named values (`CBR`/`VBR`) improves readability and forward compatibility.

## 7. Project Directory Structure

```text
hwcodec_core/
  CMakeLists.txt
  README.md
  apps/
    hw_encode_jpeg.cpp
    hw_decode_h265.cpp
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

## 8. Build Guide

## 8.1 Prerequisites

- C++14 toolchain
- CMake >= 3.16
- OpenCV (with imgcodecs)
- FFmpeg with Rockchip encoder enabled (`hevc_rkmpp`)
- Rockchip MPP (`rockchip_mpp.pc`)
- librga (`librga.so`)

## 8.2 Build

```bash
cd hwcodec_core
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

## 8.3 Smoke Test

```bash
./build/hw_encode_jpeg --input in.jpg --output out.h265 --width 1920 --height 1080
./build/hw_decode_h265 --input out.h265 --output out.jpg --width 1920 --height 1080
```

## 9. Public API Usage

### 9.1 Encoder API

Header: `include/hwcodec_core/encoder.hpp`

Typical flow:
1. create `Encoder`
2. call `init(config)`
3. call `encode_bgr(...)` or `encode_jpeg(...)`
4. consume `EncodedPacket.payload`

Minimal example:

```cpp
hwcodec_core::EncoderConfig cfg;
cfg.width = 1920;
cfg.height = 1080;
cfg.fps = 25;

hwcodec_core::Encoder enc;
if (!enc.init(cfg)) {
  // handle error
}

hwcodec_core::EncodedPacket pkt;
if (enc.encode_jpeg(jpeg.data(), jpeg.size(), pkt)) {
  // pkt.payload contains H.265 elementary stream bytes
}
```

### 9.2 Decoder API

Header: `include/hwcodec_core/decoder.hpp`

Typical flow:
1. create `Decoder`
2. call `init(config)`
3. call `decode_to_bgr(...)` or `decode_to_jpeg(...)`

Minimal example:

```cpp
hwcodec_core::DecoderConfig cfg;
cfg.width = 1920;
cfg.height = 1080;
cfg.jpeg_quality = 80;

hwcodec_core::Decoder dec;
if (!dec.init(cfg)) {
  // handle error
}

hwcodec_core::EncodedPacket in;
in.payload = h265_bytes;

std::vector<uint8_t> jpeg;
if (dec.decode_to_jpeg(in, jpeg)) {
  // jpeg contains encoded JPEG image
}
```

## 10. Runtime And Integration Notes

- Keep ROS node as transport wrapper only.
- Put codec business logic in `libhwcodec_core`.
- Use fixed dependency root in deployment image.
- Do not rely on global mutable `LD_LIBRARY_PATH`.

## 11. Known Risks Before Production

- color conversion path must be fixed (`imcopy -> imcvtcolor`)
- MPP split mode timing must be corrected
- decode polling robustness should be improved
- stride-safe conversion path should be added
- runtime loader strategy (RPATH or launcher) should be validated on target device

## 12. Recommended Next Actions

1. Fix the 3 highest-priority issues in section 4.1/4.2/4.3.
2. Add unit/integration tests:
   - BGR stride != width*3
   - split-mode streaming bitstream input
   - decode queue backpressure behavior
3. Add CI build on target Linux image with real Rockchip runtime libraries.

## 13. External References

- MPP official repository: https://github.com/rockchip-linux/mpp
- librga official repository: https://github.com/airockchip/librga
- ffmpeg-rockchip repository (used in this workspace): https://github.com/nyanmisaka/ffmpeg-rockchip
