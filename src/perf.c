#include "perf.h"

#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vitacompanion_kernel.h>

/* FPS and CPU load are averaged over this window. */
#define PERF_WINDOW_US (500 * 1000)

typedef struct {
    vitacompanion_perf kernel;
    SceKernelSystemInfo system;
    SceUInt64 time_us;
} perf_sample;

static int take_sample(perf_sample *sample)
{
    int result;

    memset(sample, 0, sizeof(*sample));
    result = vitaCompanionKernelGetPerf(&sample->kernel);
    if (result < 0)
        return result;

    sample->system.size = sizeof(sample->system);
    result = sceKernelGetSystemInfo(&sample->system);
    sample->time_us = sceKernelGetProcessTimeWide();
    return result;
}

/* Frames per second in tenths, rounded. */
static uint32_t fps_tenths(uint32_t frames, SceUInt64 elapsed_us)
{
    return (uint32_t)(((SceUInt64)frames * 10000000 + elapsed_us / 2) /
        elapsed_us);
}

/* Share of the window the core was not idle, as PSVshellPlus computes it. */
static int cpu_load(SceUInt64 idle_us, SceUInt64 elapsed_us)
{
    int64_t load = 100 - (int64_t)(idle_us * 100 / elapsed_us);

    if (load < 0)
        return 0;
    if (load > 100)
        return 100;
    return (int)load;
}

int perf_format(char *res_msg, size_t res_size)
{
    perf_sample before;
    perf_sample after;
    SceUInt64 elapsed_us;
    size_t used;
    int load[4];
    int result;
    int i;

    result = take_sample(&before);
    if (result >= 0)
    {
        sceKernelDelayThread(PERF_WINDOW_US);
        result = take_sample(&after);
    }
    if (result < 0)
    {
        snprintf(res_msg, res_size,
            "Error: cannot read performance data (0x%08X).\n", result);
        return result;
    }

    elapsed_us = after.time_us - before.time_us;
    for (i = 0; i < 4; ++i)
        load[i] = cpu_load(after.system.cpuInfo[i].idleClock -
            before.system.cpuInfo[i].idleClock, elapsed_us);

    snprintf(res_msg, res_size,
        "clocks: cpu=%d bus=%d gpu_core=%d gpu_mp=%d gpu_xbar=%d MHz\n"
        "cpu_load: %d%% %d%% %d%% %d%%\n",
        after.kernel.cpu_mhz, after.kernel.bus_mhz, after.kernel.gpu_core_mhz,
        after.kernel.gpu_mp_mhz, after.kernel.gpu_xbar_mhz,
        load[0], load[1], load[2], load[3]);

    used = strlen(res_msg);
    if (after.kernel.frames_counted)
    {
        uint32_t app = fps_tenths(after.kernel.frames[0] -
            before.kernel.frames[0], elapsed_us);
        uint32_t shell = fps_tenths(after.kernel.frames[1] -
            before.kernel.frames[1], elapsed_us);

        snprintf(res_msg + used, res_size - used,
            "fps: app=%u.%u shell=%u.%u\n",
            (unsigned int)(app / 10), (unsigned int)(app % 10),
            (unsigned int)(shell / 10), (unsigned int)(shell % 10));
    }
    else
        snprintf(res_msg + used, res_size - used, "fps: unavailable\n");

    used = strlen(res_msg);
    /* The application whose frames the app FPS counts. */
    if (after.kernel.app_pid > 0)
    {
        char title_id[16] = { 0 };
        char title[128] = { 0 };

        /* Param 9 is the full TITLE; 10 is the short STITLE. */
        sceAppMgrAppParamGetString(after.kernel.app_pid, 9, title,
            sizeof(title));
        if (sceAppMgrGetNameById(after.kernel.app_pid, title_id) >= 0)
            snprintf(res_msg + used, res_size - used, "app: %s %s\n",
                title_id, title);
        else
            snprintf(res_msg + used, res_size - used, "app: pid 0x%08X\n",
                (unsigned int)after.kernel.app_pid);
    }
    else
        snprintf(res_msg + used, res_size - used, "app: none\n");

    used = strlen(res_msg);
    if (sceKernelIsPSVitaTV())
        snprintf(res_msg + used, res_size - used, "battery: none\n");
    else
    {
        int temp = scePowerGetBatteryTemp();

        snprintf(res_msg + used, res_size - used,
            "battery: %d%% %d.%dC %dmV%s\n",
            scePowerGetBatteryLifePercent(), temp / 100, temp % 100 / 10,
            scePowerGetBatteryVolt(),
            scePowerIsBatteryCharging() ? " charging" : "");
    }

    return 0;
}
