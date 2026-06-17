#ifndef CM7_FILE_INDEX_H
#define CM7_FILE_INDEX_H

#include "stdint.h"

#define MAX_FILES     128
#define MAX_NAME_LEN  64

typedef struct
{
    char filename[MAX_NAME_LEN];
    uint32_t filesize;
} FILE_INFO;

typedef struct
{
    volatile uint32_t update_request;
    volatile uint32_t update_busy;
    volatile uint32_t update_done;
    volatile uint32_t file_count;
    FILE_INFO files[MAX_FILES];
} SHARED_FILE_LIST;

/*
 * 指向 CM7 的 Shared RAM
 */
#define SHARED_FILE_LIST_ADDR  ((SHARED_FILE_LIST *)0x30040000)

#endif