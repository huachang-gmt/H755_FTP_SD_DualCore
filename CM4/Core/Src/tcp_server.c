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
static uint16_t ftp_rx_index = 0;
static uint8_t data_connected = 0;
volatile uint8_t ftp_transfer_busy = 0;
static char dir_data[8192];

static ip_addr_t active_ip;
static uint16_t active_port = 0;
static uint8_t active_mode = 0;

typedef enum {
    FTP_CMD_NONE = 0,
    FTP_CMD_LIST,
    FTP_CMD_NLST,
    FTP_CMD_RETR
} ftp_cmd_t;

static ftp_cmd_t pending_cmd = FTP_CMD_NONE;

static err_t tcp_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);      
static err_t pasv_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err);                                
static err_t active_connect_callback(void *arg, struct tcp_pcb *tpcb, err_t err);
static void ftp_active_connect(void);


static err_t ftp_data_sent_callback(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    // 進入此處代表 FileZilla 已成功收到我們發過去的目錄或檔案資料
    
    // 清除該 PCB 的所有回呼，防止重複觸發
    //tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    
    if (tpcb == data_client_pcb)
    {
        data_client_pcb = NULL;
        data_connected = 0;
    }

    // 安全關閉資料通道 (資料已確認送達，此時關閉非常安全)
    tcp_close(tpcb);

    // 資料安全傳完並關閉後，向「控制通道」發送 226 狀態碼
    if(control_client_pcb != NULL)
    {
        const char *msg226 = "226 Transfer complete\r\n";
        tcp_write(control_client_pcb, msg226, strlen(msg226), TCP_WRITE_FLAG_COPY);
        tcp_output(control_client_pcb);
    }

    return ERR_OK;
}

static err_t data_recv_callback(
                    void *arg,
                    struct tcp_pcb *tpcb,
                    struct pbuf *p,
                    err_t err)
{
    if(p == NULL)
    {
        tcp_close(tpcb);

        if(data_client_pcb == tpcb)
        {
            data_client_pcb = NULL;
        }

        data_connected = 0;
        ftp_transfer_busy = 0;

        if(control_client_pcb)
        {

            tcp_write(control_client_pcb,
                      "226 Transfer complete\r\n",
                      23,
                      TCP_WRITE_FLAG_COPY);

            tcp_output(control_client_pcb);
        }

        return ERR_OK;
    }

    tcp_recved(tpcb,p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}

// 要注意，如果檔案數量增加，要修改 CM4\Core\Inc\cm7_file_index.h 與 CM7\Core\Inc\cm7_file_index.h 最大邊限數值
// 讀取 CM7 SD 卡檔案列表並發送
static void send_dir_list(void)
{   
    if(data_client_pcb == NULL || ftp_transfer_busy) return;

    ftp_transfer_busy = 1;

    volatile SHARED_FILE_LIST *fs = (volatile SHARED_FILE_LIST *)SHARED_FILE_LIST_ADDR;
    fs->update_done = 0;
    fs->update_request = 1; // 通知 CM7 進行檔案系統檢查    

    // 讀取 CM7 資料
    uint32_t timeout_ms = 1000; // 給 CM7 充足的 1 秒鐘時間去讀 SD 卡
    while(fs->update_done == 0)  // CM7 的 CM7_BuildFileList() 執行完成後，此旗標會在 CM7 main 設定為 1
    {        
        HAL_Delay(1); // 每次等 1 毫秒，釋放總線
        if(--timeout_ms == 0)
        {
            ftp_transfer_busy = 0;
            if(control_client_pcb)
            {
                tcp_write(control_client_pcb, "451 Shared memory timeout\r\n", 27, TCP_WRITE_FLAG_COPY);
                tcp_output(control_client_pcb);
            }
            tcp_close(data_client_pcb);
            data_client_pcb = NULL;
            return;
        }
    }

    // fs->update_done = 1 是由 CM7 CM7_BuildFileList() 執行完成後，由 CM7 main 程序內設定
    // fs->file_count 計算檔案數量是正確的，由 CM7 CM7_BuildFileList() 計算得到

    uint32_t offset = 0;
    memset(dir_data, 0, sizeof(dir_data));

    if(fs->file_count == 0)
    {
        strcpy(dir_data,
               "-rw-r--r-- 1 root root 0 EMPTY.TXT\r\n");
        offset = strlen(dir_data);
    }
    else
    { 

        for(uint32_t i = 0; i < fs->file_count; i++)
        {   
            if(fs->files[i].filename[0] == '\0' || fs->files[i].filename[0] == 0xFF)
            {
                continue; // 檔名是空的，直接跳過這個壞檔案
            }

            size_t name_len = strlen((const char *)fs->files[i].filename);
            if(name_len == 0 || name_len > 255) 
            {
                continue; // 檔名長度不合理，跳過
            }

            if(strlen((const char *)fs->files[i].filename) == 0) continue;
            
            
            int len = snprintf(
                    &dir_data[offset],
                    sizeof(dir_data) - offset,
                    "-rw-r--r-- 1 root root %lu Jan 01 2026 %s\r\n",
                    (unsigned long)fs->files[i].filesize,
                    (const char *)fs->files[i].filename);
            

            /*  使用這種方法 可以 無礙 顯示檔案列表
            int len = snprintf(
                    &dir_data[offset],
                    sizeof(dir_data) - offset,
                    "-rw-r--r-- 1 root root %lu %ld\r\n",
                    (unsigned long)fs->files[i].filesize,
                    i);
            */



/*          使用這種方法 會斷線，連不上，或是 刷新時會失敗，除非 修改 for(uint32_t i = 0; i < fs->file_count-1; i++)
            int len = snprintf(
                    &dir_data[offset],
                    sizeof(dir_data) - offset,
                    "-rw-r--r-- 1 root root %lu %s\r\n",
                    (unsigned long)fs->files[i].filesize,
                    (const char *)fs->files[i].filename);
*/
            

            if(len <= 0 || (offset + len) >= sizeof(dir_data))
            {                
                // 緩衝區滿了，防範溢出
                break;
            }

            offset += len;

        }
    }
  
    
    tcp_sent(data_client_pcb, ftp_data_sent_callback);

    err_t err = tcp_write(data_client_pcb, dir_data, offset, TCP_WRITE_FLAG_COPY);

    if(err == ERR_MEM) {
        // 記憶體不足（通常是 -1）-> 亮黃燈
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_1, GPIO_PIN_SET);// 亮黃燈 (PE1)
    }

    if(err != ERR_OK)
    {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);// 亮紅燈 (PB14)

        ftp_transfer_busy = 0;
        tcp_close(data_client_pcb);
        data_client_pcb = NULL;
        return;
    }

    tcp_output(data_client_pcb);
}

static void send_test_file(void)
{
    if (data_client_pcb == NULL) return;

    ftp_transfer_busy = 1;
    const char *file_data = "Hello STM32 FTP Server\r\n";

    tcp_sent(data_client_pcb, ftp_data_sent_callback);

    err_t err = tcp_write(data_client_pcb, file_data, strlen(file_data), TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK)
    {
        ftp_transfer_busy = 0;
        return;
    }
    tcp_output(data_client_pcb);

}

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

static err_t tcp_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{   
    control_client_pcb = newpcb; 
    tcp_recv(newpcb, tcp_recv_callback);                  

    const char *msg = "220 STM32 FTP Server Ready\r\n";
    tcp_write(newpcb, msg, strlen(msg), TCP_WRITE_FLAG_COPY);
    tcp_output(newpcb);

    return ERR_OK;
}

static err_t tcp_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if(p == NULL)
    {
        if(tpcb == control_client_pcb) control_client_pcb = NULL;
        ftp_rx_index = 0;
        memset(ftp_rx_buffer, 0, sizeof(ftp_rx_buffer));
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
                // data port number ： 7 × 256 + 228 = 2020
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
                    pending_cmd = (strncmp(ftp_rx_buffer, "LIST", 4) == 0) ? FTP_CMD_LIST : FTP_CMD_NLST;

                    // 若已連線成功（如主動模式已建立，或被動連線已先完成三向交握）則直接發送
                    if(data_client_pcb != NULL && data_connected == 1)
                    {
                        const char *msg150 = "150 Opening ASCII mode data connection for file list\r\n";
                        tcp_write(tpcb, msg150, strlen(msg150), TCP_WRITE_FLAG_COPY);
                        tcp_output(tpcb);

                        send_dir_list();
                        pending_cmd = FTP_CMD_NONE; 
                    }
                    else
                    {
                        if(active_mode == 1)
                        {
                            const char *msg150 = "150 Opening ASCII mode data connection for file list\r\n";
                            tcp_write(tpcb, msg150, strlen(msg150), TCP_WRITE_FLAG_COPY);
                            tcp_output(tpcb);
                            ftp_active_connect(); // 驅動主動式連線
                        }
                        // 被動模式下如果 data_connected == 0，代表 FileZilla 還在做 2020 交握，完全交給 pasv_accept_callback 處理
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
    if(active_mode == 0) return;

    if(data_client_pcb != NULL)
    {
        tcp_close(data_client_pcb);
        data_client_pcb = NULL;
    }

    data_client_pcb = tcp_new();    
    if(data_client_pcb == NULL) return;

    tcp_connect(data_client_pcb, &active_ip, active_port, active_connect_callback);   
}

// 移除原有銷毀式的 `ftp_pasv_init` 函式，避免在運作中反覆破壞綁定
/*
static void pasv_data_err_callback(void *arg, err_t err)
{
    if (data_client_pcb != NULL)
    {
        data_client_pcb = NULL;
    }
    data_connected = 0;
    ftp_transfer_busy = 0;

}
*/
// 被動資料通道 (Port 2020) 接收到客戶端連入時的回呼
static err_t pasv_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{

    if (err != ERR_OK || newpcb == NULL)
    {
        return ERR_VAL;
    }

    data_client_pcb = newpcb;
    data_connected = 1;

    tcp_arg(newpcb,NULL);

    tcp_recv(newpcb,data_recv_callback);

    //tcp_err(newpcb,pasv_data_err_callback);

    // 當 FileZilla 完成被動模式 Port 2020 握手連入時，補發 150 訊號並傳送 SD 檔案列表資料
    if(pending_cmd == FTP_CMD_LIST || pending_cmd == FTP_CMD_NLST)
    {
        if(control_client_pcb != NULL)
        {
            const char *msg150 = "150 Opening ASCII mode data connection for file list\r\n";
            tcp_write(control_client_pcb, msg150, strlen(msg150), TCP_WRITE_FLAG_COPY);
            tcp_output(control_client_pcb);
        }
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
    if(err != ERR_OK) return err;

    data_connected = 1;

    if(pending_cmd == FTP_CMD_LIST || pending_cmd == FTP_CMD_NLST)
    {
        send_dir_list();
        pending_cmd = FTP_CMD_NONE;
    }
    else if(pending_cmd == FTP_CMD_RETR)
    {
        send_test_file();
        pending_cmd = FTP_CMD_NONE;
    }

    return ERR_OK;
}