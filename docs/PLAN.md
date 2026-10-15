# nnnix 開發計畫

**nnnix**：NuvotoN NUC UNIX（NuvotoN Nuc NIX）。跑在 NU-LB_002（NUC140VE3CN）上的小型類 UNIX 作業系統。

建立：2026-10-15。狀態：**M1 完成**（2026-10-15），下一步 M2。

## 1. 目標

- 類 UNIX 使用體驗：shell、檔案系統樹、`/dev` 裝置、行程、`ls`／`cat`／`ps` 這類指令。
- 三種 console 同時可用：**telnet（W5500）**、**板載 LCD + USB 鍵盤（CH9350）**、**CP2102 序列埠**。
- **搶佔式多工**：多個 shell session 與背景服務同時執行。
- 程式模型：**先內建**（指令編進韌體）→ **後期可載入**（從隨身碟／網路載入獨立程式）。
- 能自動化測試：NanoPi 燒錄 → telnet 登入 → 跑指令 → 比對輸出。

不做：記憶體保護（Cortex-M0 沒有 MMU／MPU）、多使用者權限的安全性、POSIX 完整相容、圖形介面。

## 2. 硬體資源與限制

| 資源 | 規格 | nnnix 用途 |
| --- | --- | --- |
| CPU | Cortex-M0，50.19 MHz（PLL from HIRC，已驗證） | 無除法指令、無 MPU |
| SRAM | **16 KB** | 最大限制，見 §4 預算 |
| APROM | 128 KB | kernel + 驅動 + shell + 內建指令 |
| LDROM | 4 KB | 後期：bootloader／更新器（選做） |
| W5500 | 硬體 TCP/IP，8 socket，晶片內 32 KB 緩衝，SPI1 ≤25 MHz | 網路、telnet；緩衝不佔 MCU RAM |
| CH376S | USB 隨身碟，晶片內建 FAT32，SPI1 ≤12.5 MHz | `/usb`；**同時只能開一個檔案** |
| CH9350 | USB 鍵鼠 → UART1 PB4（只接收） | 鍵盤輸入、滑鼠事件 |
| CP2102 | UART0 PB1 TX | kernel log；雙向需解決 PB0 衝突（R3） |
| 板載 LCD | 128×64 單色，SPI3（PD8～PD11、PD14） | 文字 console，約 21×8 字 |
| 板載 SPI Flash | W25X16 2 MB，SPI2（PD0～PD5） | 根檔案系統 `/` |
| 其他板載 | LED（PC12）、蜂鳴器、按鍵、I²C EEPROM 24C64、RTC | `/dev/led`、`/dev/beep`、設定、時間 |

已知硬體限制（來自前期測試）：
- CH376S 必須逐 byte 傳 SPI；32-bit 連續傳輸會壞資料。
- PB4／PB5 與板載 LIN 共用：LIN 必須維持 normal idle、PB5 固定高；CH9350 只能單向接收。
- PB0（UART0 RX）可能被板載 RS-232 收發器驅動，CP2102 TXD 目前不能直接接。
- APROM 執行中**不能改寫 APROM 自己**（FMC ISPFF：APROM writes to itself）；只能改寫 LDROM。

## 3. 架構

```
 使用者層   nsh（shell）  內建指令  telnetd  getty   ← 之後：載入的程式
 ─────────── SVC 系統呼叫（內建與載入的程式用同一套 ABI）────────────
 kernel     排程器  行程／task  wait queue／mutex  計時器  klog  panic
            VFS（fd 表、掛載表） tty／行編輯  pipe  socket 層
 檔案系統   rootfs（W25X16）  usbfs（CH376S）  devfs  procfs
 驅動       uart  spi（匯流排鎖）  gpio／exti  ch9350  ch376  w5500  lcd  w25x16  rtc
 arch       啟動、時脈、中斷、PendSV context switch、SysTick、HardFault dump
```

設計重點：
- **單一位址空間、無保護**。行程 = task（獨立堆疊）+ fd 表 + cwd + 環境變數。
- **所有程式都經 SVC 呼叫 kernel**。內建指令也走 libc → SVC，這樣後期載入的程式不用改 ABI。
- **排程**：固定優先權 + 同優先權 round-robin；SysTick 1 ms，時間片 10 ms；PendSV 切換 context。
- **驅動用中斷 + wait queue**：UART1 RX、W5500 INT（PB2）、CH376 INT（PB14）以 GPIO 中斷喚醒等待的 task，不輪詢。
- **SPI1 匯流排鎖**：CH376S 與 W5500 共用。CH376S 等待 USB 時（等 INT）不佔匯流排。
- **CH376S 一次只能開一個檔**：usbfs 記住每個 fd 的檔名與位置，切換時重新開檔並 seek。之後可加快取。
- **W5500 緩衝在晶片內**：socket 讀寫直接在 W5500 與使用者緩衝之間搬，不另配 MCU 緩衝。
- **LCD 不用 framebuffer**（BSP 的 1 KB 太貴）：只存 21×8 字元格（168 B），逐字直接寫 LCD 的 page／column。
- **堆疊溢位偵測**：沒有 MPU，改用堆疊填色 + 底部 canary，排程時檢查。
- **當機資訊**：HardFault 把暫存器和堆疊印到 UART0，存進 `.noinit` RAM 後重開機；開機時回報上次當機。

## 4. 記憶體預算

SRAM 16 KB（初估，M1 實測後修正）：

| 項目 | 大小 |
| --- | --- |
| kernel .data／.bss（task 表、fd、掛載表、驅動狀態） | ~2.5 KB（M1 實測 3.5 KB） |
| 中斷／kernel 堆疊 | 1 KB（M1 實測最高 556 B） |
| idle + 系統 task（telnetd、kbd/tty、服務） | ~2 KB |
| 使用者行程堆疊：最多 4 個 × 1～1.5 KB | ~5 KB |
| tty 緩衝（3 console + 行編輯）、pipe（128 B ×2） | ~1 KB |
| 檔案系統（littlefs cache 3×256 B + 共用 512 B 區塊緩衝） | ~1.5 KB |
| 動態配置池（固定大小區塊） | 剩餘 ~3 KB |

Flash 128 KB 目標：kernel + 驅動 ≤ 45 KB、檔案系統 ≤ 20 KB、shell + 內建指令 ≤ 40 KB、字型 ≤ 1 KB，保留 ≥ 20 KB。

## 5. 腳位與周邊配置

| 周邊 | 腳位 | 狀態 |
| --- | --- | --- |
| UART0 TX → CP2102 | PB1 | 已驗證 |
| UART0 RX ← CP2102 | PB0 | 待解決（R3） |
| UART1 RX ← CH9350 | PB4；PB5／PB6／PB7 固定高（LIN idle） | 已驗證 |
| SPI1 CLK／MISO／MOSI | PC9／PC10／PC11 | 已驗證 |
| CH376S CS／INT | PC8／PB14 | 已驗證 |
| W5500 CS／INT／RST | PB10／PB2／PB8 | 已驗證 |
| SPI2 → W25X16 | PD0～PD5（板載） | 待驗證（R2） |
| SPI3 → LCD、LCD 電源 | PD8～PD11、PD14（板載） | 待驗證（R1） |
| LED | PC12 | 已驗證 |
| 蜂鳴器、按鍵、EEPROM、RTC 晶振 | 依板子資料 | 待驗證（R4） |

## 6. 檔案系統配置

```
/            rootfs：W25X16 + littlefs（斷電安全）
/bin         （M6 起）載入式程式
/etc         rc、net.conf、motd、passwd（簡易）
/home        使用者檔案
/dev         devfs：tty0（LCD+鍵盤）tty1/2（telnet）ttyS0（序列埠）null zero
             kbd mouse lcd led beep spiflash usb net
/proc        ps、meminfo、uptime、net（唯讀文字）
/usb         CH376S 隨身碟 FAT32（插入時掛載）
/tmp         RAM 中的小檔（選做，可能因 RAM 不足而省略）
```

## 7. 里程碑

每個里程碑都要有自動測試，才算完成。

### M0 基礎建設
- 建立 nnnix repo（結構見 §8）：clang/lld 建置、linker script、啟動、時脈、klog（UART0）。
- 把 `firmware/nuc140_app` 已驗證的驅動移植成 `drivers/`。
- HardFault dump + `.noinit` 當機紀錄。
- `tools/`：flash、序列埠 log 擷取、telnet 自動測試框架（expect 風格，跑在 NanoPi）。
- `tests/host/`：與硬體無關的模組在 Linux 上單元測試（CH9350 解析、行編輯、shell 解析器）。

**驗收**：`make flash test` 一次完成燒錄 → 開機 banner → 刻意觸發 fault → 重開後回報。

**結果（2026-10-15）**：`make test` 11/11 通過。程式 11.9 KB、靜態 RAM 2.7 KB、堆疊最高 0.8 KB／2 KB。
過程中確認的硬體行為：
- W5500 的 `Sn_TX_WR`／`Sn_TX_FSR` 讀回的是**上次 SEND 時**的值。SEND 前連續寫入，要在軟體裡累計已排入的量，否則後寫的會蓋掉前面的資料（驅動用 `queued[]` 處理）。
- 同一時間只能有一個 SEND；前一個還沒 SEND_OK 就送下一個，下一個會被忽略。
- 對方網卡的 link 還在建立時收到 SYN，唯一的監聽 socket 會卡在 SYN_RECV 約 32 秒。診斷服務超過 2 秒就重新監聽；M2 的 socket 層要內建這個處理，並準備多個監聽 socket。

### M1 kernel 核心
- task 建立／結束、PendSV context switch、SysTick、sleep、優先權排程。
- wait queue、mutex、semaphore、事件；SVC 系統呼叫入口與編號表。
- 固定區塊配置器；堆疊 canary；kernel `ps` 輸出（先印到 klog）。

**驗收**：3 個 task 以不同週期執行並互相搶佔；刻意讓堆疊溢位會被偵測；RAM 實測數字回填 §4。

**結果（2026-10-15）**：`make test` 15/15、`make host-test` 82 項通過。
- 兩個同優先權 CPU hog 分到 509／491 ms（比例 0.96）；同時跑的高優先權 10 ms ticker 最差延遲 0 ms。
- 堆疊溢位：saved sp 進入堆疊底部 128 B 紅區（或底部 canary 被改）時 panic，訊息存入 `.noinit`，重開後回報。
- 系統呼叫：SVC handler 只改寫堆疊上的返回位址，kernel 函式在呼叫者自己的 task context 執行，因此可以阻塞。
- 實測 RAM（16 KB）：靜態 3.5 KB（含 10 個 TCB）、MSP 1 KB（最高 556 B）、`.noinit` 512 B、heap 11.6 KB。
  系統 task 堆疊用量／配置：idle 80／384、input 160／768、diag 756／1280、mon 524／896；heap 剩 8.5 KB。
- 主機測試在 64-bit 上抓到 kmem header 大小寫死 8 bytes 的問題，已改成依結構大小計算；M0 上不受影響。

### M2 telnet console（第一個互動介面）
- W5500 改成中斷驅動 + socket 層（`socket/bind/listen/accept/send/recv/close`）。
- tty 層：行編輯（退格、Ctrl-C、Ctrl-D）、telnet 選項協商（最少 echo／SGA）。
- `telnetd` + 最小 `nsh`：`help echo ps free uptime reboot dmesg`。
- 網路設定先沿用靜態 198.18.0.2/30；DHCP 留到 M5。

**驗收**：NanoPi 同時開 2 個 telnet session 各自執行指令；自動測試腳本通過。

### M3 本機 console：LCD + 鍵盤
- LCD 驅動：初始化、5×7 字型、21×8 文字、捲動、游標。
- CH9350 鍵盤：HID 鍵碼 → ASCII、Shift／CapsLock（軟體狀態）、方向鍵 → ANSI 序列；`/dev/kbd`、`/dev/mouse`。
- `tty0` = LCD + 鍵盤，開機在 `tty0` 起 `nsh`。

**驗收**：用鍵盤打字、LCD 顯示並執行指令；同時 telnet session 正常（多工）。

### M4 檔案系統
- VFS：路徑解析、掛載表、每行程 fd 表、`open/read/write/lseek/close/stat/readdir/mkdir/unlink/rename`。
- devfs；W25X16 驅動 + littlefs 當 `/`；CH376S 的 usbfs 掛在 `/usb`；procfs。
- 指令：`ls cat cp mv rm mkdir rmdir touch df pwd cd`，shell 重新導向 `> >> <`。

**驗收**：隨身碟與 `/` 之間複製檔案，斷電重開後內容仍在；兩個 session 同時讀不同的 USB 檔案。

### M5 使用者環境與網路工具
- shell：pipe（kernel ring buffer）、`&` 背景執行、`jobs/kill`、環境變數、腳本、開機跑 `/etc/rc`。
- 指令：`ed`（行編輯器，適合 21×8 LCD）`hexdump wc head tail grep sleep date kill`。
- 網路：`ifconfig`、DHCP client、`netstat`、`nc`、簡易 HTTP 或 TFTP（NanoPi ↔ nnnix 傳檔）。
- RTC：`date`，可選 NTP 校時。

**驗收**：`/etc/rc` 開機自動設定網路並啟動 telnetd；從 NanoPi 上傳檔案到 `/home`。

### M6 可載入程式
- 執行檔格式 `NXE`：標頭 + 位置無關程式碼（clang `-fropi -frwpi`，R9 當資料基底）+ relocation。
- 載入器：從檔案系統讀進 RAM 執行，上限約 4 KB；`exec`／`spawn` 系統呼叫。
- SDK：`sdk/`（crt0、libc 子集標頭、linker script、Makefile），在 NanoPi 上編譯。

**驗收**：在 NanoPi 編譯 `hello.nxe` → 上傳 → `/bin/hello` 執行並正常結束，RAM 回收。

### M7 選做
- 序列埠雙向 console（依 R3 結果）。
- LDROM bootloader：從 W25X16 讀取新映像寫入 APROM，支援 `nnupdate` 網路更新 kernel，以及把程式「安裝」到 APROM 原地執行（XIP）。**需要修改 CONFIG0 的開機選擇，必須先經使用者同意。**
- watchdog、低電量／斷電處理、簡易登入（`/etc/passwd`）。

## 8. 原始碼結構

```
nnnix/
  Makefile            make / make flash / make test / make host-test
  arch/nuc140/        startup.c  vectors  link.ld  clock.c  irq.c  switch.S  fault.c
  kernel/             sched.c task.c sync.c syscall.c timer.c klog.c panic.c kmem.c
  drivers/            uart.c spi.c gpio.c ch9350.c ch376.c w5500.c lcd.c w25x16.c rtc.c
  fs/                 vfs.c devfs.c procfs.c usbfs.c rootfs.c pipe.c  third_party/littlefs/
  net/                socket.c telnetd.c dhcp.c
  lib/                string.c printf.c（kernel 與使用者共用）
  user/               nsh/  cmd/（每個內建指令一個檔）
  sdk/                （M6）
  tests/host/         Linux 上的單元測試
  tools/              flash.py  console.py  nntest.py（telnet 自動測試）
```

Repo：<https://github.com/willie6666/nnnix>，每個里程碑至少 commit 一次。

## 9. 待研究（開始實作前或對應里程碑前完成）

| 編號 | 項目 | 影響 |
| --- | --- | --- |
| R1 | LCD 控制器型號、初始化序列、PD14 電源控制（BSP `Library/Nu-LB-NUC140/Source/LCD.c`） | M3 |
| R2 | W25X16 是否在板上、JEDEC ID、SPI2 腳位（BSP `SampleCode/Nu-LB-NUC140/SPI_Flash`） | M4 |
| R3 | PB0 的 RS-232 收發器型號與隔離方法；備案：軟體 UART RX 或改用其他腳位 | M7 |
| R4 | RTC 32.768 kHz 晶振、蜂鳴器、按鍵、EEPROM 腳位 | M5 |
| R5 | littlefs 在 Cortex-M0 的程式碼大小與 RAM 用量；超出則改自製簡單 FS | M4 |
| R6 | clang 對 thumbv6m 的 ROPI／RWPI 支援是否可用 | M6 |
| R7 | W5500 中斷（Sn_IMR／SIMR）+ PB2 GPIO 中斷實測 | M2 |

## 10. 風險

| 風險 | 對策 |
| --- | --- |
| 16 KB RAM 不夠 | M1 起每個里程碑量測 RAM；行程數與堆疊大小可設定；`/tmp` 可省略 |
| 沒有記憶體保護，一個程式就能弄壞 kernel | 堆疊 canary、HardFault 紀錄、watchdog 自動重開 |
| CH376S 單檔限制導致多 session 存取很慢 | usbfs 重開檔快取；USB 主要用於搬檔，日常檔案放 `/` |
| LIN 共用再次拉低 PB4 | 沿用已驗證的 LIN normal idle 設定，寫進 arch 初始化 |
| 寫壞 Flash 無法開機 | 原 APROM 有備份；只有 M7 會動 CONFIG／LDROM，且需同意 |
| 鍵盤輸入無法自動化 | 自動測試以 telnet 為主；鍵盤解析用錄下的 CH9350 封包做單元測試 |

## 11. 下一步

從 **M0** 開始：建立 `nnnix/` 結構與建置流程，移植現有驅動，完成 fault dump 與自動測試框架。
