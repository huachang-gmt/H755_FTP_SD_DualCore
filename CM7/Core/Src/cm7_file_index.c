#include "cm7_file_index.h"
#include "fatfs.h"
#include "string.h"

/*
 * 這個變數會被 linker script 放到：
 * .shared_fs → RAM_D2 (0x30040000)
 */
SHARED_FILE_LIST g_shared_file_list
__attribute__((section(".shared_fs")));


uint32_t CM7_BuildFileList(void)
{
    DIR dir;
    FILINFO fno;
    FRESULT res;

    uint32_t idx = 0;

    // 初始化 shared memory
    memset((void*)&g_shared_file_list, 0, sizeof(g_shared_file_list));

    res = f_opendir(&dir, "/LOGFILES");
    if (res != FR_OK)
        return 0;

    while (1)
    {
        res = f_readdir(&dir, &fno);

        if (res != FR_OK || fno.fname[0] == 0)
            break;

        if (fno.fattrib & AM_DIR)
            continue;

        strncpy(
             (char*)g_shared_file_list.files[idx].filename,
            fno.fname,
            MAX_NAME_LEN - 1);

        g_shared_file_list.files[idx].filename[MAX_NAME_LEN - 1] = '\0';

        g_shared_file_list.files[idx].filesize = fno.fsize;

        idx++;

        if (idx >= MAX_FILES)
            break;
    }

    g_shared_file_list.file_count = idx;



    /* 強制把 Shared RAM 寫回實體記憶體 */
    SCB_CleanDCache_by_Addr(
        (uint32_t *)&g_shared_file_list,
        sizeof(g_shared_file_list));

/*  檢查共享位址是否正確
if((uint32_t)&g_shared_file_list == 0x30040000)
{
    HAL_GPIO_WritePin(GPIOB,
                      GPIO_PIN_14,
                      GPIO_PIN_SET); // 實驗結果 ： 紅燈亮
}
else
{
    HAL_GPIO_WritePin(GPIOE,
                      GPIO_PIN_1,
                      GPIO_PIN_SET);
}
*/
/*
if(idx == 12)  // 卻認為 12 個檔案 在 SD 卡的 資料夾 LOGFILES 內
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);// 亮綠燈 (PB0)
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_1, GPIO_PIN_SET);// 亮黃燈 (PE1)
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);// 亮紅燈 (PB14)
    // 正確，三個 LED 都會亮
}
*/
    f_closedir(&dir);

    return idx;   // ⭐新增：回傳檔案數
}


/*
 * CM7：掃 SD 卡 → 建立檔案列表 → 寫入 shared memory
 */
/*
void CM7_BuildFileList(void)
{
    DIR dir;
    FILINFO fno;
    FRESULT res;

    uint32_t idx = 0;

    // 初始化
    memset((void*)&g_shared_file_list, 0, sizeof(g_shared_file_list));

    // 開 root directory
    res = f_opendir(&dir, "/");
    if (res != FR_OK)
        return;

    while (1)
    {
        res = f_readdir(&dir, &fno);

        // error or end
        if (res != FR_OK || fno.fname[0] == 0)
            break;

        // skip directory
        if (fno.fattrib & AM_DIR)
            continue;

        // copy filename to shared memory
        strncpy(g_shared_file_list.file_list[idx],
                fno.fname,
                MAX_NAME_LEN - 1);

        idx++;

        if (idx >= MAX_FILES)
            break;
    }

    g_shared_file_list.file_count = idx;

    f_closedir(&dir);
}
*/