#pragma once

#include <stdint.h>

#define VITACOMPANION_KERNEL_ABI_VERSION 4
#define VITACOMPANION_KERNEL_MODULE_NAME "vitacompanion_kernel"
/* This syscall lost its fifth, stack-passed argument on device. */
#define VITACOMPANION_TOUCH_ACTIVE_FLAG 0x100

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixelformat;
} vitacompanion_screen_info;

typedef struct {
    int cpu_mhz;
    int bus_mhz;
    int gpu_core_mhz;
    int gpu_mp_mhz;
    int gpu_xbar_mhz;
    /* 0 if the display hook could not be installed. */
    int frames_counted;
    /* Flips of the foreground application's and the shell's framebuffer. */
    uint32_t frames[2];
    /* Process that owns the application framebuffer, or 0 if none does. */
    int32_t app_pid;
} vitacompanion_perf;

int vitaCompanionKernelGetApiVersion(void);
int vitaCompanionKernelSetButtons(uint32_t buttons, int pressed);
int vitaCompanionKernelSetAnalog(int stick, int x, int y, int active);
int vitaCompanionKernelSetTouch(int port, int slot_and_active, int x, int y);
int vitaCompanionKernelReset(void);
/*
 * Describes the displayed frame and returns its size in bytes, which is
 * width * height * 4. If dst holds that many bytes, the next frame the app
 * submits is copied there without tearing, or the current frame if none is
 * submitted within 100 ms.
 */
int vitaCompanionKernelScreenCapture(vitacompanion_screen_info *info,
    void *dst, uint32_t dst_size);
/* Current clocks and frame counters since the module started. */
int vitaCompanionKernelGetPerf(vitacompanion_perf *perf);
