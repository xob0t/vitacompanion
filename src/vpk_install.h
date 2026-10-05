#pragma once

#include "vpk_package.h"

#include <stddef.h>

int vpk_install(const char *vpk_path,
    char title_id[VPK_TITLE_ID_LENGTH + 1]);
int vpk_install_from(vpk_read_fn read, void *ctx,
    char title_id[VPK_TITLE_ID_LENGTH + 1]);
int vpk_install_format_error(int error, char *buffer, size_t buffer_size);
