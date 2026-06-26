#ifndef CM7_FILE_INDEX_H
#define CM7_FILE_INDEX_H

#include "stdint.h"

#define MAX_FILES     128
#define MAX_NAME_LEN  64
#define MAX_DOWNLOAD_BYTE 1024


// 計算 32 byte 對齊後的長度
#define ALIGN_32_SIZE(size) (((size) + 31) & ~31)

/*
 * 這個 struct 是 CM7 → CM4 共享資料
 * 必須完全一致（CM4 之後會用）
 */

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
    volatile uint8_t sd_dropped;   // 【新增】1: 代表 SD 卡被拔除，控制網路端主動斷線
    
    volatile uint32_t retr_request;
    volatile uint32_t retr_busy;
    volatile uint32_t retr_done;
    char retr_filename[64];
    uint32_t retr_size;
    uint8_t retr_buffer[MAX_DOWNLOAD_BYTE];

    FILE_INFO files[MAX_FILES];
} SHARED_FILE_LIST;

/* 外部共享變數 */
extern SHARED_FILE_LIST g_shared_file_list;

/* CM7 建立檔案列表 API */
uint32_t CM7_BuildFileList(void);
uint32_t CM7_ReadFile512Bytes(const char *filename, uint8_t *buffer);
uint32_t CM7_ReadFileRequest(SHARED_FILE_LIST *fs);

#endif