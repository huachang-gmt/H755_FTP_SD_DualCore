#include "cm7_file_index.h"
#include "fatfs.h"
#include "string.h"

/*
 * 這個變數會被 linker script 放到：
 * .shared_fs → RAM_D2 (0x30040000)
 */
// 在宣告變數時，同時指定 section 與 32-byte 對齊
SHARED_FILE_LIST g_shared_file_list 
__attribute__((section(".shared_fs"), aligned(32)));

uint32_t CM7_BuildFileList(void)
{
    DIR dir;
    FILINFO fno;
    FRESULT res;

    uint32_t idx = 0;    
    
    // 初始化 shared memory
    memset((void*)g_shared_file_list.files, 0, sizeof(g_shared_file_list.files));
    g_shared_file_list.file_count = 0;

    res = f_opendir(&dir, "/LOGFILES"); // 檔案列表的根目錄
    if (res != FR_OK)
        return 0;

    while (1)
    {
        res = f_readdir(&dir, &fno);

        if (res != FR_OK || fno.fname[0] == 0)
            break;

        if (fno.fattrib & AM_DIR)
            continue;

        if (idx >= MAX_FILES)
            break;
        
        
        strncpy(
             (char*)g_shared_file_list.files[idx].filename,
            fno.fname,
            MAX_NAME_LEN - 1);

        g_shared_file_list.files[idx].filename[MAX_NAME_LEN - 1] = '\0';
        
        g_shared_file_list.files[idx].filesize = fno.fsize;

        idx++;        
    }

    g_shared_file_list.file_count = idx; // 檔案數量

    /* 強制把 Shared RAM 寫回實體記憶體 */
    SCB_CleanDCache_by_Addr(
        (uint32_t *)&g_shared_file_list,
        ALIGN_32_SIZE(sizeof(g_shared_file_list)));
        
    // 共享位址 if((uint32_t)&g_shared_file_list == 0x30040000)

    f_closedir(&dir);

    return idx;   // 回傳檔案數
}


uint32_t CM7_ReadFile512Bytes(const char *filename, uint8_t *buffer)
{
    FIL file;
    FRESULT res;
    UINT br;

    char fullpath[128];

    snprintf(fullpath, sizeof(fullpath), "/LOGFILES/%s", filename);

    res = f_open(&file, fullpath, FA_READ);

    if(res != FR_OK)
    {
        return 0;
    }

    res = f_read(&file, buffer, MAX_DOWNLOAD_BYTE, &br);

    f_close(&file);

    if(res != FR_OK)
    {
        return 0;
    }

    return br;
}

uint32_t CM7_ReadFileRequest(SHARED_FILE_LIST *fs)
{
    FIL file;
    FRESULT res;
    UINT br;

    char path[128];

    snprintf(path, sizeof(path), "/LOGFILES/%s", fs->retr_filename);

    res = f_open(&file, path, FA_READ);
    if(res != FR_OK)
    {
        fs->retr_size = 0;
        return 0;
    }

    res = f_read(&file, fs->retr_buffer, sizeof(fs->retr_buffer), &br);

    f_close(&file);

    fs->retr_size = br;

    SCB_CleanDCache_by_Addr(
        (uint32_t *)fs->retr_buffer,
        ALIGN_32_SIZE(sizeof(fs->retr_buffer)));

    return br;
}
