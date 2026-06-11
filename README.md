# STM32H755ZI-Q FTP Server + SD 卡 檔案系統 整合

## 開發進度追蹤


[2026-06-11] 完成 CM4 FTP Server 建置，可以透過 Filezilla Client 連接上。連接輸入 IP ： 192.168.88.10 使用者名稱： anonymous， 密碼 ： test 連接埠： 21 (可以不填) ，按下 快速連線，右側會出現 開發板端的 虛擬(模擬) 檔案列表，有兩個檔案： log.txt 與 test.txt 。 

[2026-06-10] Ethernet 網路建置完成，從電腦端以網路線接到開發板 RJ45， ping 192.168.88.10 成功。 


## 專案簡介

## Ethernet 網路部分

### 本專案使用 STM32H755ZI-Q NUCLEO 開發板 NUCLEO-H755ZI-Q 建立 Ethernet 網路功能，採用：

* STM32H755ZIT6
* Cortex-M4
* LAN8742A PHY
* RMII（Reduced Media Independent Interface）介面
* STM32 HAL Ethernet Driver
* LwIP TCP/IP Stack
* TCP Server using the LwIP Raw API
* FTP Control Channel port ： 21
* FTP Passive Mode (PASV)

## 最終目標：

* 建立穩定 Ethernet 通訊
* Ping 測試成功
* 在 CM4 核心 建立 TCP server 與 FTP Server 

---

```text
FileZilla Client
        │
        ▼
STM32 FTP Server
        │
        ▼
LwIP TCP/IP Stack
        │
        ▼
Ethernet Driver
        │
        ▼
RMII PHY
```

---

# Relationship to the OSI Model

The project gradually covers multiple layers of the OSI network model.

| OSI Layer            | Protocol / Function |
| -------------------- | ------------------- |
| Layer 7 Application  | FTP                 |
| Layer 6 Presentation | FTP Text Commands   |
| Layer 5 Session      | FTP Control Session |
| Layer 4 Transport    | TCP                 |
| Layer 3 Network      | IP / ICMP           |
| Layer 2 Data Link    | Ethernet / ARP      |
| Layer 1 Physical     | RMII PHY            |

---


---

# 開發環境 

MCU： STM32H755ZIT6

開發板： NUCLEO-H755ZI-Q

PHY： LAN8742A

網路堆疊： LwIP

開發工具：

* STM32CubeMX
* STM32CubeIDE

執行核心： Cortex-M4

---

## SD 卡 存取 部分 

## 專案目標： 
* 將 STM32H7 以 Raw Sector 方式高速寫入 SD 卡的資料 
* 重新讀出後，轉換成 Windows 可辨識的 FATFS 檔案。

# 系統設計概念

本系統分成兩個階段：

---

# 第一階段：高速 Raw Data 寫入 SD 卡

目標：

> 不透過 FATFS，
> 直接以 Sector 方式高速寫入 SD 卡。

---

# 第二階段：Raw Data 生成 FATFS 檔案

本專案第二階段：

> 將 SD 卡內 Raw Data 重新讀出，
> 並生成 Windows 可見檔案。

---
