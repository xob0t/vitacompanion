#pragma once

#include <stdint.h>

int cmd_thread(unsigned int args, void* argp);
int cmd_start();
void cmd_end();
void cmd_request_reboot();
int cmd_payload_start(uint32_t size);
int cmd_payload_read(void* ctx, void* buffer, unsigned int size);
void cmd_payload_finish(void);
