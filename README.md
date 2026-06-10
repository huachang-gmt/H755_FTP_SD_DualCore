# STM32H755ZI-Q FTP Server + SD 卡 檔案系統 整合

## 開發進度追蹤

[2026-06-10] Ethernet 網路建置完成，從電腦端以網路線接到開發板 RJ45， ping 192.168.88.10 成功。 








## 專案簡介

### 本專案使用 STM32H755ZI-Q NUCLEO 開發板 NUCLEO-H755ZI-Q 建立 Ethernet 網路功能，採用：

* STM32H755ZIT6
* Cortex-M4
* LAN8742A PHY
* RMII（Reduced Media Independent Interface）介面
* STM32 HAL Ethernet Driver
* LwIP TCP/IP Stack

### 本專案使用 STM32H755ZI-Q NUCLEO 開發板 NUCLEO-H755ZI-Q 
* 將 STM32H7 以 Raw Sector 方式高速寫入 SD 卡的資料 
* 重新讀出後，轉換成 Windows 可辨識的 FATFS 檔案。
* 
* 
* 
* 

## 最終目標：

* 建立穩定 Ethernet 通訊
* Ping 測試成功
* 在 CM4 核心 建立 TCP server 與 FTP Server 

---

# 開發環境 

MCU：

STM32H755ZIT6

開發板：

NUCLEO-H755ZI-Q

PHY：

LAN8742A

網路堆疊：

LwIP

開發工具：

* STM32CubeMX
* STM32CubeIDE

執行核心：

* Cortex-M4


# 系統設計概念


