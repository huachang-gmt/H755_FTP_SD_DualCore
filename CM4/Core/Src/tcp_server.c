#include "tcp_server.h"
#include "string.h"
#include "strings.h"
#include "lwip/tcp.h"
#include "main.h"
#include "stdio.h"
#include "cm7_file_index.h"
#include "lwip/netif.h"

#define FTP_PASV_PORT 2020

static struct tcp_pcb *server_pcb = NULL;
static struct tcp_pcb *pasv_pcb = NULL;           // 修改點：維持常駐，不重設 bind，避免 Refresh 斷線
struct tcp_pcb *data_client_pcb = NULL;
struct tcp_pcb *control_client_pcb = NULL; // 記錄目前的控制通道客戶端

static char ftp_rx_buffer[256];
static char retr_filename[64];
static uint16_t ftp_rx_index = 0;
static uint8_t data_connected = 0;
volatile uint8_t ftp_transfer_busy = 0;
static char dir_data[8192];

static ip_addr_t active_ip;
static uint16_t active_port = 0;
static uint8_t active_mode = 0;

static uint32_t total_dir_len = 0;   // 總資料長度
static uint32_t sent_dir_len = 0;    // 已發送資料長度

typedef enum {
    FTP_CMD_NONE = 0,
    FTP_CMD_LIST,
    FTP_CMD_NLST,
    FTP_CMD_RETR
} ftp_cmd_t;

static ftp_cmd_t pending_cmd = FTP_CMD_NONE;
static uint8_t list_waiting_cm7 = 0;
static uint32_t list_wait_start_tick = 0;

static err_t tcp_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);      
static err_t pasv_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err);                                
static err_t active_connect_callback(void *arg, struct tcp_pcb *tpcb, err_t err);
static void ftp_active_connect(void);

static void ftp_send_dir_list_after_cm7_ready(void);

static err_t ftp_data_sent_callback(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    // 進入此處代表 FileZilla 已成功收到我們發出的資料 (ACK)
    
    // 【重要提醒】：如果未來你要實作「大檔案下載 (RETR)」，且檔案需要切成好幾個 Chunk 分次 tcp_write，
    // 這裡必須加上判斷式，確認「檔案是否真的全部傳完了」，才能執行下面的關閉動作。
    // 如果是傳送目錄列表，因為是一次性 tcp_write，直接關閉沒問題。
    sent_dir_len += len;

    // 1. 如果還有資料沒傳完，繼續傳下一段
    if (sent_dir_len < total_dir_len) {
        u32_t remaining = total_dir_len - sent_dir_len;
        u16_t snd_buf = tcp_sndbuf(tpcb);
        u16_t send_len = (u16_t)(remaining > snd_buf ? snd_buf : remaining);

        if (tcp_write(tpcb, &dir_data[sent_dir_len], send_len, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            tcp_output(tpcb);
            return ERR_OK; // 繼續等待下一次回調
        }
        
    }

    // 這裡會被執行，進入此處代表 FileZilla 已成功收到我們發出的資料 (ACK)
    //HAL_GPIO_TogglePin(GPIOE, GPIO_PIN_1); // 黃燈閃爍 (PE1)

    // 2. 全部傳完後的清理邏輯 (進入此處代表已完成)
    ftp_transfer_busy = 0;

    // 1. 清除該 PCB 的所有回呼，防止 LwIP 核心後續觸發不可預期的存取
    tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_recv(tpcb, NULL);    // 新增：確保收到意外封包時不會報錯
    tcp_err(tpcb, NULL);     // 新增：解除錯誤處理回呼
    tcp_poll(tpcb, NULL, 0); // 新增：解除輪詢

    // 2. 清除全域變數指標 (整合你原本重複寫了兩次的邏輯)
    if (tpcb == data_client_pcb)
    {
        data_client_pcb = NULL;
        data_connected = 0;
    }

    // 3. 安全關閉資料通道 (加入 LwIP 必備的錯誤處理機制)
    err_t err = tcp_close(tpcb);
    if (err != ERR_OK)
    {
        // 如果 LwIP 此時剛好記憶體池耗盡，無法發出 FIN 封包，tcp_close 會失敗並回傳 ERR_MEM。
        // 若不處理，這個連線會變成僵屍連線 (Memory Leak)。因此必須強制中止並釋放 PCB。
        tcp_abort(tpcb);
        
        // 呼叫 tcp_abort 後，必須回傳 ERR_ABRT 告訴 LwIP 核心這個 pcb 已經銷毀，請勿再操作。
        return ERR_ABRT; 
    }

    // 4. 資料安全傳完並關閉後，向「控制通道」發送 226 狀態碼
    if (control_client_pcb != NULL && control_client_pcb->state == ESTABLISHED) {
        tcp_write(control_client_pcb, "226 Transfer complete\r\n", 23, TCP_WRITE_FLAG_COPY);
        tcp_output(control_client_pcb);
    }

    pending_cmd = FTP_CMD_NONE;

    return ERR_OK;
}

static err_t data_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    // 檔案列表時，不會執行這裡，應該是檔案下載時走這裡
    //HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0); // 綠燈閃爍 (PB0)

    if(p == NULL)
    {
        // 【修改點】：清除回呼函數
        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_err(tpcb, NULL);
        tcp_sent(tpcb, NULL);
        tcp_poll(tpcb, NULL, 0);

        tcp_close(tpcb);

        if(data_client_pcb == tpcb)
        {
            data_client_pcb = NULL;
        }

        data_connected = 0;
        ftp_transfer_busy = 0;

        if(control_client_pcb)
        {
            tcp_write(control_client_pcb, "226 Transfer complete\r\n", 23, TCP_WRITE_FLAG_COPY);
            tcp_output(control_client_pcb);
        }

        return ERR_OK;
    }
    //HAL_GPIO_TogglePin(GPIOE, GPIO_PIN_1); // 黃燈閃爍 (PE1)

    tcp_recved(tpcb,p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}


static void send_dir_list(void)
{
    volatile SHARED_FILE_LIST *fs = (volatile SHARED_FILE_LIST *)SHARED_FILE_LIST_ADDR;

    if(data_client_pcb == NULL)
    {
        return;
    }

    if(ftp_transfer_busy)
    {
        return;
    }

    // 每一次按下 refresh 或是首次 要索取檔案列表，都會經過這裡，檔案列表必執行之處
    //HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0);
    //HAL_GPIO_TogglePin(GPIOE, GPIO_PIN_1);
    //HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_14);

    ftp_transfer_busy = 1;

    fs->update_done = 0;// 先清除旗標，當 CM7 檔案列表準備好且傳送過來後，CM7 會設定此旗標為 1
    fs->update_busy = 0;

    fs->update_request = 1; // 這個旗標會透過與 CM7 共用 share memory  讓 CM7 的 主迴圈獲知狀態改變

    list_waiting_cm7 = 1;// 已經送出 request 檔案列表 給 CM7 ，等待 CM7 回覆

    list_wait_start_tick = HAL_GetTick(); // 等待計時開始
   

}

static void ftp_send_dir_list_after_cm7_ready(void)
{
    if(data_client_pcb == NULL)
    {
        ftp_transfer_busy = 0;
        return;
    }

    volatile SHARED_FILE_LIST *fs =
        (volatile SHARED_FILE_LIST *)SHARED_FILE_LIST_ADDR;

    uint32_t offset = 0;

    memset(dir_data,0,sizeof(dir_data));

    if(fs->file_count == 0)
    {
        strcpy(dir_data,
               "-rw-r--r-- 1 root root 0 EMPTY.TXT\r\n");

        offset = strlen(dir_data);
    }
    else
    {
        for(uint32_t i=0;i<fs->file_count;i++)
        {
            if(fs->files[i].filename[0]=='\0')
            {
                continue;
            }

            // 新增安全過濾機制
            char clean_name[MAX_NAME_LEN];
            uint32_t j = 0;
            for(j = 0; j < MAX_NAME_LEN - 1; j++)
            {
                char c = fs->files[i].filename[j];
                if(c == '\0') break;
                
                // 唯有標準可列印 ASCII 碼 (32~126) 才保留，其餘記憶體髒資料、亂碼或非英文字元一律轉為下底線
                if(c >= 32 && c <= 126) {
                    clean_name[j] = c;
                } else {
                    clean_name[j] = '_';
                }
            }
            clean_name[j] = '\0';
            
            // 如果過濾完不幸變成長度為 0 的空字串，就跳過
            if(strlen(clean_name) == 0) continue;
            // 檔案列表的顯示格式
            int len =
                snprintf(
                    &dir_data[offset],
                    sizeof(dir_data)-offset,
                    "-rw-r--r-- 1 root root %lu Jan 01 2026 %s\r\n",
                    (unsigned long)fs->files[i].filesize,
                    clean_name);

            if(len <= 0)
            {
                continue;
            }

            if(offset + len >= sizeof(dir_data))
            {
                break;
            }

            offset += len;
        }
    }

    total_dir_len = offset;// 累積 檔案列表 總長度
    sent_dir_len = 0;

    tcp_sent(data_client_pcb, ftp_data_sent_callback);

    u16_t send_len = (u16_t)(total_dir_len > tcp_sndbuf(data_client_pcb) ? 
                             tcp_sndbuf(data_client_pcb) : total_dir_len);

    err_t err = tcp_write(data_client_pcb, dir_data, send_len, TCP_WRITE_FLAG_COPY);

    if (err == ERR_OK) {
        // 執行這裡，檔案列表
        //HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0); // 綠燈閃爍 (PB0)
        sent_dir_len = send_len;
        tcp_output(data_client_pcb);
    } else {
        //HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_14); // 紅燈閃爍  (PB14)
        // 發生錯誤則清理
        ftp_transfer_busy = 0;
        tcp_close(data_client_pcb);
        data_client_pcb = NULL;
    }
}

// 此函數式 處於主迴圈 執行
void ftp_poll_cm7_list_ready(void)
{
    if(list_waiting_cm7 == 0)
    {
        return;
    }

    volatile SHARED_FILE_LIST *fs =
        (volatile SHARED_FILE_LIST *)SHARED_FILE_LIST_ADDR;

    // CM7 通知 檔案列表已經準備完成
    if(fs->update_done)
    {
        //HAL_GPIO_TogglePin(GPIOE, GPIO_PIN_1); // 黃燈閃爍 (PE1)

        list_waiting_cm7 = 0;

        ftp_send_dir_list_after_cm7_ready();

        return;
    }

    if((HAL_GetTick() - list_wait_start_tick) > 3000)
    {
        list_waiting_cm7 = 0;
        ftp_transfer_busy = 0;

        if(control_client_pcb)
        {
            tcp_write(control_client_pcb,
                      "451 LIST timeout\r\n",
                      18,
                      TCP_WRITE_FLAG_COPY);

            tcp_output(control_client_pcb);
        }

        if(data_client_pcb)
        {
            tcp_close(data_client_pcb);
            data_client_pcb = NULL;
        }
    }
}


static void send_test_file(void)
{
    if (data_client_pcb == NULL)
    {
        return;
    }

    ftp_transfer_busy = 1;

    volatile SHARED_FILE_LIST *fs =
        (volatile SHARED_FILE_LIST *)SHARED_FILE_LIST_ADDR;

    /* 清除舊狀態 */
    fs->retr_done = 0;
    fs->retr_busy = 0;
    fs->retr_size = 0;

    memset((void*)fs->retr_buffer,
           0,
           sizeof(fs->retr_buffer));

    strncpy((char*)fs->retr_filename,
            retr_filename,
            MAX_NAME_LEN - 1);

    fs->retr_filename[MAX_NAME_LEN - 1] = 0;

    /* 送出讀檔要求 */
    fs->retr_request = 1;

    /*
     * ★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★
     * 不再等待 retr_done
     * 直接返回
     * 讓 lwIP 繼續跑
     * ★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★
     */
}



/*
static void send_test_file(void)
{
    if (data_client_pcb == NULL) return;

    ftp_transfer_busy = 1;


    volatile SHARED_FILE_LIST *fs =
        (volatile SHARED_FILE_LIST *)SHARED_FILE_LIST_ADDR;

    
    fs->retr_done = 0;
    fs->retr_busy = 0;

    strncpy((char*)fs->retr_filename,
            retr_filename,
            MAX_NAME_LEN - 1);

    fs->retr_request = 1;

    uint32_t timeout = 3000;

    while(fs->retr_done == 0)
    {
        HAL_Delay(1);

        if(--timeout == 0)
        {
            ftp_transfer_busy = 0;
            return;
        }
    }

    tcp_sent(data_client_pcb, ftp_data_sent_callback);

    tcp_write(data_client_pcb,
              (const void *)fs->retr_buffer,
              fs->retr_size,
              TCP_WRITE_FLAG_COPY);

    tcp_output(data_client_pcb);
}
*/

void tcp_server_init(void)
{
    ip_addr_t ipaddr;
    IP_ADDR4(&ipaddr, 0, 0, 0, 0);

    // 1. 初始化 Port 21 控制通道監聽
    server_pcb = tcp_new();
    if(server_pcb != NULL) {
        if(tcp_bind(server_pcb, &ipaddr, 21) == ERR_OK) {
            server_pcb = tcp_listen(server_pcb);
            tcp_accept(server_pcb, tcp_accept_callback);
        }
    }

    // 修改點：將被動模式 Port 2020 的監聽在此處初始化，建立為常駐監聽，永不重覆關閉綁定
    pasv_pcb = tcp_new();
    if(pasv_pcb != NULL) {
        if(tcp_bind(pasv_pcb, &ipaddr, FTP_PASV_PORT) == ERR_OK) {
            pasv_pcb = tcp_listen(pasv_pcb);
            tcp_accept(pasv_pcb, pasv_accept_callback);
        }
    }
}


// 處理控制通道被 FileZilla 強制中斷的狀況
static void control_err_callback(void *arg, err_t err)
{
    // LwIP 已經在底層把 PCB 殺掉了，我們絕對不能呼叫 tcp_close
    // 只能乖乖把指標設為 NULL，避免後續寫入引發 HardFault
    if (control_client_pcb != NULL)
    {
        control_client_pcb = NULL;
    }
}

// 處理資料通道被 FileZilla 強制中斷的狀況
static void data_err_callback(void *arg, err_t err)
{
    if (data_client_pcb != NULL)
    {
        data_client_pcb = NULL;
    }
    data_connected = 0;
    ftp_transfer_busy = 0;

}


static err_t tcp_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{   

    if(err != ERR_OK || newpcb == NULL) return ERR_VAL;

    // 【修改點】：防呆，如果前一個控制連線還在，強行中斷它，避免 PCB 覆蓋導致後續存取崩潰
    if (control_client_pcb != NULL) {
        tcp_abort(control_client_pcb);
        control_client_pcb = NULL;
    }

    control_client_pcb = newpcb; 
    
    tcp_arg(newpcb, NULL);                  // 確保沒有傳入錯誤的 arg
    tcp_err(newpcb, control_err_callback);  // <---- 【新增】：綁定錯誤回呼
    tcp_recv(newpcb, tcp_recv_callback);       

    const char *msg = "220 STM32 FTP Server Ready\r\n";
    tcp_write(newpcb, msg, strlen(msg), TCP_WRITE_FLAG_COPY);
    tcp_output(newpcb);    

    // Filezilla 連上 第一站

    return ERR_OK;
}

static err_t tcp_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if(p == NULL)
    {
        if(tpcb == control_client_pcb) control_client_pcb = NULL;
        ftp_rx_index = 0;
        memset(ftp_rx_buffer, 0, sizeof(ftp_rx_buffer));

        // 【修改點】：關閉前徹底清除回呼函數，確保 LwIP 核心後續不會再觸發存取
        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_err(tpcb, NULL);
        tcp_sent(tpcb, NULL);
        tcp_poll(tpcb, NULL, 0);

        tcp_close(tpcb);
        return ERR_OK;
    }

    char *rx_data = (char *)p->payload;

    for(uint16_t i = 0; i < p->tot_len; i++)
    {
        if(ftp_rx_index < sizeof(ftp_rx_buffer)-1)
        {
            ftp_rx_buffer[ftp_rx_index++] = rx_data[i];
        }

        if(rx_data[i] == '\n')
        {
            ftp_rx_buffer[ftp_rx_index] = 0;
            const char *reply = NULL; 

            if(strncmp(ftp_rx_buffer, "USER", 4) == 0) reply = "331 Password required\r\n";
            else if(strncmp(ftp_rx_buffer, "PASS", 4) == 0) reply = "230 Login successful\r\n";
            else if(strncmp(ftp_rx_buffer, "SYST", 4) == 0) reply = "215 UNIX Type: L8\r\n";
            else if(strncmp(ftp_rx_buffer, "FEAT", 4) == 0)
            {
                const char *feat_reply = "211-Features\r\n UTF8\r\n PASV\r\n211 End\r\n";
                tcp_write(tpcb, feat_reply, strlen(feat_reply), TCP_WRITE_FLAG_COPY);
                tcp_output(tpcb);
                reply = NULL;
            }
            else if(strncmp(ftp_rx_buffer, "TYPE", 4) == 0) reply = "200 Type set OK\r\n";
            else if(strncmp(ftp_rx_buffer, "XPWD", 4) == 0 || strncmp(ftp_rx_buffer, "PWD", 3) == 0) reply = "257 \"/\" is current directory\r\n";
            else if(strncmp(ftp_rx_buffer, "QUIT", 4) == 0) reply = "221 Goodbye\r\n";
            else if(strncmp(ftp_rx_buffer, "NOOP", 4) == 0) reply = "200 OK\r\n";
            else if(strncmp(ftp_rx_buffer, "OPTS", 4) == 0) reply = "200 UTF8 enabled\r\n";
            else if(strncmp(ftp_rx_buffer, "PASV", 4) == 0)
            {                 
                // 修改點：因為 pasv_pcb 已在初期化時常駐監聽 Port 2020，這裡完全不重設監聽
                active_mode = 0;            // 確保切換為被動模式
                pending_cmd = FTP_CMD_NONE; // 清空遺留指令

                if(data_client_pcb != NULL)
                {
                    tcp_arg(data_client_pcb, NULL);
                    tcp_sent(data_client_pcb, NULL);
                    tcp_recv(data_client_pcb, NULL); // <---- 新增
                    tcp_err(data_client_pcb, NULL);  // <---- 新增
                    tcp_close(data_client_pcb);
                    data_client_pcb = NULL;
                }
                ftp_transfer_busy = 0; 
                data_connected = 0;

                /*
                // 以下程式段是針對 正式版本時，使用 DHCP 模式。目前先讓 Filezilla 可以連線，先暫時關閉
                // 針對 DHCP 時，彈性變動 Reply 
                struct netif *netif = netif_default;

                if(netif != NULL)
                {
                    uint8_t h1 = ip4_addr1(ip_2_ip4(&netif->ip_addr));
                    uint8_t h2 = ip4_addr2(ip_2_ip4(&netif->ip_addr));
                    uint8_t h3 = ip4_addr3(ip_2_ip4(&netif->ip_addr));
                    uint8_t h4 = ip4_addr4(ip_2_ip4(&netif->ip_addr));

                    uint16_t port = FTP_PASV_PORT;

                    uint8_t p1 = port >> 8;
                    uint8_t p2 = port & 0xFF;

                    snprintf(pasv_reply,
                            sizeof(pasv_reply),
                            "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n",
                            h1,
                            h2,
                            h3,
                            h4,
                            p1,
                            p2);

                    reply = pasv_reply;
                }
                else
                {
                    reply = "425 Cannot determine IP address\r\n";
                }
                */           
                
                // 直接給予被動模式成功連入回覆即可，不呼叫銷毀重建的 ftp_pasv_init()
                reply = "227 Entering Passive Mode (192,168,88,10,7,228)\r\n";
                // data port number ： 7 × 256 + 228 = 2020  << Data port 數值由來

                // Filezilla 連上 第二站

            }
            else if(strncmp(ftp_rx_buffer, "PORT", 4) == 0)
            {
                // 保留點：完整維持主動式 FTP 機制與剖析邏輯
                int h1, h2, h3, h4, p1, p2;
                if(sscanf(ftp_rx_buffer, "PORT %d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) == 6)
                {
                    IP4_ADDR(&active_ip, h1, h2, h3, h4);
                    active_port = (p1 << 8) | p2;
                    active_mode = 1; // 確立主動模式
                    pending_cmd = FTP_CMD_NONE;

                    if(data_client_pcb != NULL)
                    {
                        tcp_arg(data_client_pcb, NULL);
                        tcp_sent(data_client_pcb, NULL);
                        tcp_close(data_client_pcb);
                        data_client_pcb = NULL;
                    }
                    ftp_transfer_busy = 0; 
                    data_connected = 0;

                    reply = "200 PORT command successful\r\n";
                }
                else
                {
                    reply = "501 Syntax error in PORT\r\n";
                }
            }
            else if(strncmp(ftp_rx_buffer, "LIST", 4) == 0 || strncmp(ftp_rx_buffer, "NLST", 4) == 0)
            {
                if(ftp_transfer_busy)
                {
                    reply = "450 Transfer already in progress\r\n";
                }
                else
                {
                    // Filezilla 連上 第三站

                    pending_cmd = (strncmp(ftp_rx_buffer, "LIST", 4) == 0) ? FTP_CMD_LIST : FTP_CMD_NLST;


                    // 若已連線成功（如主動模式已建立，或被動連線已先完成三向交握）則直接發送
                    if(data_client_pcb != NULL && data_connected == 1)                    
                    {
                       //有時執行這裡
                       
                       //HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0); // 綠燈閃爍 (PB0)
                       //HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_14); // 紅燈閃爍  (PB14)

                        const char *msg150 = "150 Opening ASCII mode data connection for file list\r\n";
                        tcp_write(tpcb, msg150, strlen(msg150), TCP_WRITE_FLAG_COPY);
                        tcp_output(tpcb);

                        send_dir_list();  
                        pending_cmd = FTP_CMD_NONE; // 因為 send_dir_list 已經被執行，在 下一站是 pasv_accept_callback() 中，因為 pending_cmd 關係，不會再重複執行 send_dir_list
                    }
                    else
                    {

                       // 有時執行這裡
                       //HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0); // 綠燈閃爍 (PB0)
                       //HAL_GPIO_TogglePin(GPIOE, GPIO_PIN_1); // 黃燈閃爍 (PE1)
                        
                        if(active_mode == 1)
                        {         
                            const char *msg150 = "150 Opening ASCII mode data connection for file list\r\n";
                            tcp_write(tpcb, msg150, strlen(msg150), TCP_WRITE_FLAG_COPY);
                            tcp_output(tpcb);                   
                            ftp_active_connect(); // 驅動主動式連線
                        }
                        else
                        {
                            // 這裡是被動模式執行路徑 active_mode = 0 
                            // 被動模式下如果 data_connected == 0，代表 FileZilla 還在做 2020 交握，完全交給 pasv_accept_callback 處理
                            // 下一站是 pasv_accept_callback()                             
                            //HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_14); // 紅燈閃爍  (PB14)
                        }
                        
                    }
                    reply = NULL;
                }
            }
            else if(strncmp(ftp_rx_buffer, "RETR", 4) == 0)
            {
                if(ftp_transfer_busy)
                {
                    reply = "450 Transfer already in progress\r\n";
                }
                else
                {
                    
                    memset(retr_filename, 0, sizeof(retr_filename));

                    sscanf(ftp_rx_buffer, "RETR %63s", retr_filename);                    
                    
                    pending_cmd = FTP_CMD_RETR;

                    if(data_client_pcb != NULL && data_connected == 1)
                    {
                        const char *msg150 = "150 Opening data connection\r\n";
                        tcp_write(tpcb, msg150, strlen(msg150), TCP_WRITE_FLAG_COPY);
                        tcp_output(tpcb);
                        send_test_file();
                        pending_cmd = FTP_CMD_NONE;
                    }
                    else if(active_mode == 1)
                    {
                        const char *msg150 = "150 Opening data connection\r\n";
                        tcp_write(tpcb, msg150, strlen(msg150), TCP_WRITE_FLAG_COPY);
                        tcp_output(tpcb);
                        ftp_active_connect();
                    }
                    reply = NULL;
                }               
            }
            else if(strncmp(ftp_rx_buffer,"CWD",3) == 0) reply = "250 Directory changed\r\n";
            else if(strncmp(ftp_rx_buffer,"CDUP",4)==0) reply = "200 Directory changed\r\n";
            else if(strncmp(ftp_rx_buffer,"SIZE",4)==0) reply = "213 24\r\n";
            else if(strncmp(ftp_rx_buffer,"MDTM",4)==0) reply = "213 20260609000000\r\n";
            else if(strncmp(ftp_rx_buffer,"EPSV",4)==0) reply = "500 EPSV not supported\r\n";
            else if(strncmp(ftp_rx_buffer,"EPRT",4)==0) reply = "500 EPRT not supported\r\n";
            else if(strncmp(ftp_rx_buffer,"REST",4)==0) reply = "350 Restart position accepted\r\n";
            else if(strncmp(ftp_rx_buffer,"STRU",4)==0) reply = "200 STRU F OK\r\n";
            else if(strncmp(ftp_rx_buffer,"MODE",4)==0) reply = "200 MODE S OK\r\n";
            else if(strncmp(ftp_rx_buffer,"STAT",4)==0) reply = "211 FTP Server OK\r\n";
            else if(strncmp(ftp_rx_buffer, "DELE", 4) == 0) reply = "250 File deleted\r\n";
            else if(strncmp(ftp_rx_buffer, "RNFR", 4) == 0) reply = "350 Ready for RNTO\r\n";
            else if(strncmp(ftp_rx_buffer, "RNTO", 4) == 0) reply = "250 Rename successful\r\n";
            else
            {
                reply = "500 Unknown command\r\n";
                char debug_buf[300];
                snprintf(debug_buf, sizeof(debug_buf), "500 DEBUG:%s", ftp_rx_buffer);
                tcp_write(tpcb, debug_buf, strlen(debug_buf), TCP_WRITE_FLAG_COPY);
                tcp_output(tpcb);

                ftp_rx_index = 0;
                memset(ftp_rx_buffer, 0, sizeof(ftp_rx_buffer));
                tcp_recved(tpcb, p->tot_len);
                pbuf_free(p);
                return ERR_OK;
            }

            if(reply != NULL)
            {
                tcp_write(tpcb, reply, strlen(reply), TCP_WRITE_FLAG_COPY);
                tcp_output(tpcb);
            }

            ftp_rx_index = 0;
            memset(ftp_rx_buffer, 0, sizeof(ftp_rx_buffer));
        }
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

// 主動模式專用：連線至客戶端指定的 IP 與 Port
static void ftp_active_connect(void)
{
    if(active_mode == 0) return; //  active_mode = 1 為 主動模式 FTP (Active Mode)

    if(data_client_pcb != NULL)
    {
        tcp_abort(data_client_pcb);
        data_client_pcb = NULL;
    }

    data_client_pcb = tcp_new();  
    
    if(data_client_pcb == NULL)
    {
        // 【修改點】：如果 LwIP 記憶體耗盡無法建立 PCB，回報錯誤避免 Client 死等
        if(control_client_pcb != NULL) {
            const char *msg425 = "425 Can't open data connection (Out of memory)\r\n";
            tcp_write(control_client_pcb, msg425, strlen(msg425), TCP_WRITE_FLAG_COPY);
            tcp_output(control_client_pcb);
        }
        ftp_transfer_busy = 0;
        pending_cmd = FTP_CMD_NONE;
        return;
    }    

    tcp_connect(data_client_pcb, &active_ip, active_port, active_connect_callback);   
}

static err_t data_sent_callback(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    // 這裡判斷資料是否已經全部發送完畢
    // 如果傳送完畢，則：
    tcp_close(tpcb); 
    data_client_pcb = NULL;
    data_connected = 0;

    //HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0); // 綠燈閃爍 (PB0)
    // 如果只是檔案列表，這裡不會被執行

    // 這裡補發 226
    if(control_client_pcb) {
        tcp_write(control_client_pcb, "226 Transfer complete\r\n", 23, TCP_WRITE_FLAG_COPY);
        tcp_output(control_client_pcb);
    }
    return ERR_OK;
}


// 被動資料通道 (Port 2020) 接收到客戶端連入時的回呼
static err_t pasv_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    // Filezilla 連上 第四站  
    //在 else if(strncmp(ftp_rx_buffer, "LIST", 4) == 0 || strncmp(ftp_rx_buffer, "NLST", 4) == 0) 之後，會到這裡執行，關鍵是 pending_cmd 不是 FTP_CMD_NONE 時，會執行 send_dir_list();  或是檔案下載 FTP_CMD_RETR

    if (err != ERR_OK || newpcb == NULL)
    {
        return ERR_VAL;
    }

    // 【新增防呆】：如果舊的資料通道還在，強行斬斷它，防止 Memory Leak
    if (data_client_pcb != NULL)
    {
        tcp_abort(data_client_pcb);
        data_client_pcb = NULL;
    }

    data_client_pcb = newpcb;
    data_connected = 1;

    tcp_arg(newpcb,NULL);
    tcp_err(newpcb, data_err_callback);
    tcp_recv(newpcb,data_recv_callback);
    tcp_sent(newpcb, data_sent_callback); 

    // 當 FileZilla 完成被動模式 Port 2020 握手連入時，補發 150 訊號並傳送 SD 檔案列表資料
    if(pending_cmd == FTP_CMD_LIST || pending_cmd == FTP_CMD_NLST)
    {
        if(control_client_pcb != NULL)
        {
            const char *msg150 = "150 Opening ASCII mode data connection for file list\r\n";
            tcp_write(control_client_pcb, msg150, strlen(msg150), TCP_WRITE_FLAG_COPY);
            tcp_output(control_client_pcb);
        }
        //這裡會執行
        //HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0); // 綠燈閃爍 (PB0)

        send_dir_list();            
        pending_cmd = FTP_CMD_NONE;        
    }    
    else if(pending_cmd == FTP_CMD_RETR)
    {
        if(control_client_pcb != NULL)
        {
            const char *msg150 = "150 Opening data connection\r\n";
            tcp_write(control_client_pcb, msg150, strlen(msg150), TCP_WRITE_FLAG_COPY);
            tcp_output(control_client_pcb);
        }
        send_test_file();
        pending_cmd = FTP_CMD_NONE;
    }

    return ERR_OK;
}

// 主動模式專用：與客戶端握手完成後的回呼
static err_t active_connect_callback(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    if(err != ERR_OK) {
        // 發生錯誤：清理狀態讓下次還能重試
        ftp_transfer_busy = 0;
        pending_cmd = FTP_CMD_NONE;
        data_client_pcb = NULL;
        return err;
    }

    data_connected = 1;

    if(pending_cmd == FTP_CMD_LIST || pending_cmd == FTP_CMD_NLST)
    {
        send_dir_list();
    }
    else if(pending_cmd == FTP_CMD_RETR)
    {
        send_test_file();
    }

    return ERR_OK;
}