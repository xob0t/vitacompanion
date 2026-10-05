#include "vpk_install.h"

#include "log.h"
#include "vpk_package.h"

#include <psp2/appmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>
#include <stdbool.h>
#include <stdio.h>

#define VPK_PACKAGE_DIR "ux0:data/vitacompanion_pkg"

/*
 * The promoter depends on ScePaf, which SceShell has already loaded. Load the
 * promoter only if it is missing and unload only what was loaded here, so
 * SceShell's own modules are left alone.
 */
static int promote_package(const char *path)
{
    bool loaded_here = false;
    int result;

    if (sceSysmoduleIsLoadedInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL) !=
        SCE_SYSMODULE_LOADED)
    {
        result = sceSysmoduleLoadModuleInternal(
            SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
        LOG("load promoter: 0x%08X\n", result);
        if (result < 0)
            return result;
        loaded_here = true;
    }

    result = scePromoterUtilityInit();
    LOG("scePromoterUtilityInit: 0x%08X\n", result);
    if (result >= 0)
    {
        result = scePromoterUtilityPromotePkgWithRif(path, 1);
        LOG("scePromoterUtilityPromotePkgWithRif: 0x%08X\n", result);
        scePromoterUtilityExit();
    }

    if (loaded_here)
        sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);

    return result;
}

/* Installs what was extracted to VPK_PACKAGE_DIR, then removes it. */
static int install_extracted(int result,
    char title_id[VPK_TITLE_ID_LENGTH + 1])
{
    LOG("extract: 0x%08X\n", result);
    if (result >= 0)
        result = vpk_prepare_package(VPK_PACKAGE_DIR, title_id);

    if (result >= 0)
    {
        /* Quit the application if it is running, then give it a moment. */
        if (sceAppMgrDestroyAppByName(title_id) >= 0)
            sceKernelDelayThread(1000 * 1000);
        result = promote_package(VPK_PACKAGE_DIR);
    }

    vpk_remove_tree(VPK_PACKAGE_DIR);
    return result;
}

int vpk_install(const char *vpk_path,
    char title_id[VPK_TITLE_ID_LENGTH + 1])
{
    vpk_remove_tree(VPK_PACKAGE_DIR);
    return install_extracted(vpk_extract(vpk_path, VPK_PACKAGE_DIR),
        title_id);
}

int vpk_install_from(vpk_read_fn read, void *ctx,
    char title_id[VPK_TITLE_ID_LENGTH + 1])
{
    vpk_remove_tree(VPK_PACKAGE_DIR);
    return install_extracted(vpk_extract_from(read, ctx, VPK_PACKAGE_DIR),
        title_id);
}

int vpk_install_format_error(int error, char *buffer, size_t buffer_size)
{
    const char *message = vpk_error_string(error);

    if (message)
        return snprintf(buffer, buffer_size, "Error: %s.\n", message);

    return snprintf(buffer, buffer_size,
        "Error: installation failed (0x%08X).\n", (unsigned int)error);
}
