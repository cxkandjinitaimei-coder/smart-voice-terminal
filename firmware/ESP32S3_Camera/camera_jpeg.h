/* 相机帧转 JPEG 的缓冲所有权管理。
 * 用法：一次拍照创建一个 CameraJpeg，调用 capture() 后读取 data()/size() 上传；
 * 重试复用同一对象，离开作用域自动释放 JPEG。对象不可复制，禁止手动释放其指针。
 * 原始 RGB565 帧在转换结束后立即归还驱动，网络等待不占用相机帧缓冲。
 * 本文件不负责联网、调度和相机初始化，由 WiFiCamera.ino 串行调用。
 */
#pragma once
#include <stdlib.h>
#include "esp_camera.h"
#include "img_converters.h"

class CameraJpeg {
public:
  CameraJpeg() = default;
  ~CameraJpeg() { free(bytes_); }
  CameraJpeg(const CameraJpeg &) = delete;
  CameraJpeg &operator=(const CameraJpeg &) = delete;

  bool capture() {
    // 允许同一对象重新采集；先释放上一次编码结果，失败时不能上传旧图。
    free(bytes_); bytes_ = nullptr; length_ = 0; width_ = height_ = 0;
    error_ = "";
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) { error_ = "取帧失败"; return false; }
    width_ = frame->width; height_ = frame->height;
    // 固定 RGB565 采集路径：先核对尺寸和完整长度，再交给编码器读取像素。
    bool valid = frame->buf && frame->format == PIXFORMAT_RGB565 &&
                 width_ > 0 && height_ > 0 && width_ <= 640 && height_ <= 480 &&
                 frame->len == width_ * height_ * 2;
    bool encoded = valid && frame2jpg(frame, 80, &bytes_, &length_);
    // 所有取得原始帧的路径都在这里归还一次；编码输出独立分配，仍归本对象持有。
    esp_camera_fb_return(frame);
    if (!valid) error_ = "RGB565帧格式或长度无效";
    else if (!encoded) error_ = "RGB565转JPEG失败";
    else if (!bytes_ || length_ < 4 || length_ > 4 * 1024 * 1024 ||
             bytes_[0] != 0xff || bytes_[1] != 0xd8 ||
             bytes_[length_-2] != 0xff || bytes_[length_-1] != 0xd9)
      error_ = "JPEG编码结果无效";
    else return true;
    free(bytes_); bytes_ = nullptr; length_ = 0;
    return false;
  }
  const uint8_t *data() const { return bytes_; }
  size_t size() const { return length_; }
  size_t width() const { return width_; }
  size_t height() const { return height_; }
  const char *error() const { return error_; }

private:
  uint8_t *bytes_ = nullptr;
  size_t length_ = 0, width_ = 0, height_ = 0;
  const char *error_ = "";
};
