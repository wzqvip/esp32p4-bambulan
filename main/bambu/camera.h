// A1 / P1 摄像头局域网取流：TLS 6000 端口，80 字节认证包，之后每帧 = 16 字节头 + JPEG。
// 用 P4 的硬件 JPEG 解码器解码，再用 PPA 缩放成 CAMERA_VIEW_W x CAMERA_VIEW_H 的 RGB565，
// 在 LVGL 锁内换帧，绑定到同一台打印机的所有 lv_image 一起更新。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#define CAMERA_VIEW_W 480
#define CAMERA_VIEW_H 270
#define CAMERA_MAX_BIND 2

typedef enum {
    CAMERA_OFF = 0,
    CAMERA_CONNECTING,
    CAMERA_STREAMING,
    CAMERA_REJECTED,       // 连上了但打印机不给画面（访问码错 / 未开开发者模式 / 未开局域网视频）
    CAMERA_CONN_FAILED,    // 连不上 6000 端口
    CAMERA_DECODE_FAILED,
    CAMERA_DISABLED,       // 设置里关掉了实时画面
} camera_state_t;

/** 把 lv_image 绑定到第 idx 台打印机的画面（在 LVGL 锁内、camera_start_all 之前调用）。 */
void camera_bind_image(int idx, lv_obj_t *img);

/** 网络就绪后调用：为每台已配置的打印机启动取流任务。 */
void camera_start_all(void);

camera_state_t camera_state(int idx);
const char *camera_state_text(camera_state_t st);
bool camera_has_frame(int idx);
