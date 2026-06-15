#include "tcp_server.h"
#include "string.h"
#include "strings.h"
#include "lwip/tcp.h"
#include "main.h"
#include "stdio.h"
#include "cm7_file_index.h"

static struct tcp_pcb *server_pcb = NULL;
static struct tcp_pcb *pasv_pcb = NULL;
static struct tcp_pcb *data_client_pcb = NULL;
static struct tcp_pcb *control_client_pcb = NULL; // 新增：用來記錄目前的控制通道客戶端

static char ftp_rx_buffer[256];
static uint16_t ftp_rx_index = 0;
static uint8_t data_connected = 0;

// 新增：用來記錄是否有「等待中」的傳輸指令
typedef enum {
    FTP_CMD_NONE = 0,
    FTP_CMD_LIST,
    FTP_CMD_NLST,
    FTP_CMD_RETR
} ftp_cmd_t;

static ftp_cmd_t pending_cmd = FTP_CMD_NONE;

static err_t tcp_accept_callback(void *arg,
                                 struct tcp_pcb *newpcb,
                                 err_t err);

static err_t tcp_recv_callback(void *arg,
                               struct tcp_pcb *tpcb,
                               struct pbuf *p,
                               err_t err);      
                               
static err_t pasv_accept_callback(void *arg,
                                  struct tcp_pcb *newpcb,
                                  err_t err);                               


// 新增：獨立出來的目錄資料傳送函式
static void send_dir_list(void)
{

    /*
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET); //亮紅燈 (PB14) 驗證測試用  send_dir_list 確實被執行

    // 測試實驗
    SHARED_FILE_LIST *fss =
        SHARED_FILE_LIST_ADDR;

    if(strcmp(fss->files[0].filename,
            "LOG0000.TXT") == 0)
    {
        HAL_GPIO_WritePin(GPIOE,
                        GPIO_PIN_1,
                        GPIO_PIN_SET);// 亮黃燈 (PE1)
    }
    // -----------------
    */

    if(data_client_pcb == NULL)
    {
        return;
    }

    volatile SHARED_FILE_LIST *fs = (volatile SHARED_FILE_LIST *)SHARED_FILE_LIST_ADDR;

    char dir_data[8192];

    uint32_t offset = 0;

    memset(dir_data, 0, sizeof(dir_data));

    for(uint32_t i = 0; i < fs->file_count; i++)
    {
        int len = snprintf(
                    &dir_data[offset],
                    sizeof(dir_data) - offset,
                    "-rw-r--r-- 1 root root %lu %s\r\n",
                    (unsigned long)fs->files[i].filesize,
                    fs->files[i].filename);

        if(len <= 0)
        {
            continue;
        }

        offset += len;

        if(offset > (sizeof(dir_data) - 128))
        {
            break;
        }
    }

    tcp_write(data_client_pcb,
              dir_data,
              strlen(dir_data),
              TCP_WRITE_FLAG_COPY);

    tcp_output(data_client_pcb);

    tcp_close(data_client_pcb);

    data_client_pcb = NULL;

    data_connected = 0;

    if(control_client_pcb != NULL)
    {
        const char *msg226 =
            "226 Transfer complete\r\n";

        tcp_write(control_client_pcb,
                  msg226,
                  strlen(msg226),
                  TCP_WRITE_FLAG_COPY);

        tcp_output(control_client_pcb);
    }
}














/*  虛擬檔案列表程式
static void send_dir_list(void)
{
    if(data_client_pcb != NULL)
    {
        const char *dir_data =
                    "-rw-r--r-- 1 root root 123 LOG0000.TXT\r\n"
                    "-rw-r--r-- 1 root root 123 LOG0001.TXT\r\n"
                    "-rw-r--r-- 1 root root 123 LOG0002.TXT\r\n"
                    "-rw-r--r-- 1 root root 123 LOG0003.TXT\r\n"
                    "-rw-r--r-- 1 root root 123 LOG0004.TXT\r\n"
                    "-rw-r--r-- 1 root root 123 TEST.TXT\r\n"; // 模擬檔案目錄，會顯示在 filezilla 右側

        tcp_write(data_client_pcb, dir_data, strlen(dir_data), TCP_WRITE_FLAG_COPY);
        tcp_output(data_client_pcb);

        // 傳送完畢，關閉資料通道
        tcp_close(data_client_pcb);
        data_client_pcb = NULL;
        data_connected = 0;

        // 資料傳完後，必須向「控制通道」發送 226 狀態碼
        if(control_client_pcb != NULL)
        {
            const char *msg226 = "226 Transfer complete\r\n";
            tcp_write(control_client_pcb, msg226, strlen(msg226), TCP_WRITE_FLAG_COPY);
            tcp_output(control_client_pcb);
        }
    }
}
*/

static void send_test_file(void)
{
    const char *file_data =
        "Hello STM32 FTP Server\r\n";

    tcp_write(data_client_pcb,
              file_data,
              strlen(file_data),
              TCP_WRITE_FLAG_COPY);

    tcp_output(data_client_pcb);

    tcp_close(data_client_pcb);

    data_client_pcb = NULL;

    data_connected = 0;

    if(control_client_pcb != NULL)
    {
        const char *msg226 =
            "226 Transfer complete\r\n";

        tcp_write(control_client_pcb,
                  msg226,
                  strlen(msg226),
                  TCP_WRITE_FLAG_COPY);

        tcp_output(control_client_pcb);
    }
}




void tcp_server_init(void)
{
    ip_addr_t ipaddr;

    server_pcb = tcp_new();

    if(server_pcb == NULL)
    {
        return;
    }

    IP_ADDR4(&ipaddr,0,0,0,0);

    if(tcp_bind(server_pcb,
                &ipaddr,
                21) != ERR_OK)
    {
        return;
    }

    server_pcb = tcp_listen(server_pcb);

    tcp_accept(server_pcb,
               tcp_accept_callback);
}

static err_t tcp_accept_callback(void *arg,
                                 struct tcp_pcb *newpcb,
                                 err_t err)
{   
    control_client_pcb = newpcb; // 儲存目前的控制通道 PCB
    
    tcp_recv(newpcb,
             tcp_recv_callback);                  

    const char *msg =
        "220 STM32 FTP Server Ready\r\n";

    tcp_write(newpcb,
              msg,
              strlen(msg),
              TCP_WRITE_FLAG_COPY);

    tcp_output(newpcb);


    return ERR_OK;
}


static err_t tcp_recv_callback(void *arg,
                               struct tcp_pcb *tpcb,
                               struct pbuf *p,
                               err_t err)
{
    
    if(p == NULL)
    {
        if(tpcb == control_client_pcb) control_client_pcb = NULL;

        ftp_rx_index = 0;

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

            const char *reply = NULL; // 預設改為 NULL;

            if(strncmp(ftp_rx_buffer, "USER", 4) == 0)
            {
                reply = "331 Password required\r\n";
            }
            else if(strncmp(ftp_rx_buffer, "PASS", 4) == 0)
            {
                reply = "230 Login successful\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"SYST",4)==0)
            {
                reply = "215 UNIX Type: L8\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"FEAT",4)==0)
            {
                const char *feat_reply =
                "211-Features\r\n"
                " UTF8\r\n"
                " PASV\r\n"
                "211 End\r\n";

                tcp_write(tpcb,
                        feat_reply,
                        strlen(feat_reply),
                        TCP_WRITE_FLAG_COPY);

                tcp_output(tpcb);

                reply = NULL;
            }
            else if(strncmp(ftp_rx_buffer,"TYPE",4)==0)
            {
                reply = "200 Type set OK\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"XPWD",4)==0)
            {
                reply = "257 \"/\" is current directory\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"PWD",3)==0)
            {
                reply = "257 \"/\" is current directory\r\n";
            }
            else if(strncmp(ftp_rx_buffer, "QUIT", 4) == 0)
            {
                reply = "221 Goodbye\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"NOOP",4)==0)
            {
                reply = "200 OK\r\n";
            }
            else if(strncmp(ftp_rx_buffer, "OPTS", 4) == 0)
            {
                reply = "200 UTF8 enabled\r\n";
            }
            else if(strncmp(ftp_rx_buffer, "PASV", 4) == 0)
            {
                ftp_pasv_init(); // 初始化 改從 main 移到這裡
                reply = "227 Entering Passive Mode (192,168,88,10,7,228)\r\n";
            }
            else if(strncmp(ftp_rx_buffer, "PORT", 4) == 0)
            {
                reply = "200 PORT command successful\r\n";
            }
            else if(strncmp(ftp_rx_buffer, "LIST", 4) == 0 || strncmp(ftp_rx_buffer, "NLST", 4) == 0)
            {
                // 修改：無論連線好了沒，都必須先回覆 150 告訴客戶端準備傳輸資料
                const char *msg150 = "150 Opening ASCII mode data connection for file list\r\n";
                tcp_write(tpcb, msg150, strlen(msg150), TCP_WRITE_FLAG_COPY);
                tcp_output(tpcb);

                if(data_connected == 1)
                {
                    // 如果運氣好，此時資料通道已經連線成功，直接傳送
                    send_dir_list();
                }
                else
                {
                    // 關鍵：如果資料通道還沒好（FileZilla 常見情況），先記下旗標，等連線成功後自動出發
                    pending_cmd = (strncmp(ftp_rx_buffer, "LIST", 4) == 0) ? FTP_CMD_LIST : FTP_CMD_NLST;
                }
                reply = NULL; // 設為 NULL，避免走到底部重複寫入
            }
            else if(strncmp(ftp_rx_buffer, "RETR", 4) == 0)
            {
                const char *msg150 =
                    "150 Opening data connection\r\n";

                tcp_write(tpcb,
                        msg150,
                        strlen(msg150),
                        TCP_WRITE_FLAG_COPY);

                tcp_output(tpcb);

                if(data_connected == 1)
                {
                    send_test_file();
                }
                else
                {
                    pending_cmd = FTP_CMD_RETR;
                }

                reply = NULL;
            }
            else if(strncmp(ftp_rx_buffer,"CWD",3) == 0) // 使用者雙擊目錄時
            {
                reply = "250 Directory changed\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"CDUP",4)==0) // FileZilla 常在下載前詢問檔案大小
            {
                reply = "200 Directory changed\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"SIZE",4)==0) // FileZilla 常在下載前詢問檔案大小
            {
                reply = "213 24\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"MDTM",4)==0) // FileZilla 很常詢問檔案修改時間
            {
                reply = "213 20260609000000\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"EPSV",4)==0)
            {
                reply = "500 EPSV not supported\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"EPRT",4)==0)
            {
                reply = "500 EPRT not supported\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"REST",4)==0)
            {
                reply = "350 Restart position accepted\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"STRU",4)==0)
            {
                reply = "200 STRU F OK\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"MODE",4)==0)
            {
                reply = "200 MODE S OK\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"STAT",4)==0)
            {
                reply = "211 FTP Server OK\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"STRU",4)==0)
            {
                reply = "200 STRU F OK\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"MODE",4)==0)
            {
                reply = "200 MODE S OK\r\n";
            }
            else if(strncmp(ftp_rx_buffer,"REST",4)==0)
            {
                reply = "350 Restart position accepted\r\n";
            }
            else if(strncmp(ftp_rx_buffer, "DELE", 4) == 0)
            {
                reply = "250 File deleted\r\n";
            }
            else if(strncmp(ftp_rx_buffer, "RNFR", 4) == 0)
            {
                reply = "350 Ready for RNTO\r\n";
            }
            else if(strncmp(ftp_rx_buffer, "RNTO", 4) == 0)
            {
                reply = "250 Rename successful\r\n";
            }
            else
            {
                reply = "500 Unknown command\r\n";

                // debug 使用，要抓出上面未列出 ftp 命令                
                char debug_buf[300];

                snprintf(debug_buf,
                        sizeof(debug_buf),
                        "500 DEBUG:%s",
                        ftp_rx_buffer);

                tcp_write(tpcb,
                        debug_buf,
                        strlen(debug_buf),
                        TCP_WRITE_FLAG_COPY);

                tcp_output(tpcb);

                ftp_rx_index = 0;

                tcp_recved(tpcb,
                        p->tot_len);

                pbuf_free(p);

                return ERR_OK;
                // debug 結束
                
            }

            // 只有當 reply 不為 NULL 時才進行通用的回覆
            if(reply != NULL)
            {
                tcp_write(tpcb, reply, strlen(reply), TCP_WRITE_FLAG_COPY);
                tcp_output(tpcb);
            }

            ftp_rx_index = 0;
        }
    }

    tcp_recved(tpcb,
               p->tot_len);

    pbuf_free(p);

    return ERR_OK;

}


void ftp_pasv_init(void)
{

    ip_addr_t ipaddr;

    if(pasv_pcb != NULL)
    {
        tcp_close(pasv_pcb);
        pasv_pcb = NULL;
    }

    pasv_pcb = tcp_new();

    if(pasv_pcb == NULL)
    {
        return;
    }

    IP_ADDR4(&ipaddr,0,0,0,0);

    if(tcp_bind(pasv_pcb,
                &ipaddr,
                2020) != ERR_OK)
    {
        return;
    }

    pasv_pcb = tcp_listen(pasv_pcb);

    tcp_accept(pasv_pcb,
               pasv_accept_callback);
}

static err_t pasv_accept_callback(void *arg,
                                  struct tcp_pcb *newpcb,
                                  err_t err)
{
    data_client_pcb = newpcb;

    data_connected = 1;

    // 修改：當 FileZilla 終於完成 Port 2020 連線，檢查是否有先前卡住的 LIST 指令
    if(pending_cmd == FTP_CMD_LIST || pending_cmd == FTP_CMD_NLST)
    {
        send_dir_list();        // 執行延遲傳送
        pending_cmd = FTP_CMD_NONE; // 清除旗標
    }    
    else if(pending_cmd == FTP_CMD_RETR)
    {
        send_test_file();
        pending_cmd = FTP_CMD_NONE; // 清除旗標
    }

    //HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);// 亮綠燈 (PB0)
    // 綠燈亮表示 ： PASV Port 2020  已成功建立資料通道

    return ERR_OK;
}



