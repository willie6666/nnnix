# nnnix

**NuvotoN NUC UNIX** — a tiny UNIX-like operating system for the Nuvoton NUC140 (Cortex-M0, 16 KiB SRAM, 128 KiB flash) on the NU-LB_002 board.

在 Nuvoton NUC140（Cortex-M0、16 KB SRAM、128 KB Flash）上做的小型類 UNIX 作業系統。
目標：telnet／LCD + USB 鍵盤／序列埠三種 console、搶佔式多工、檔案系統（SPI Flash + USB 隨身碟）、
之後支援載入獨立程式。完整計畫見 [docs/PLAN.md](docs/PLAN.md)。

## 狀態

**M1 kernel 核心：完成**（M0 基礎建設已完成）。

- 搶佔式多工：固定優先權 + 同優先權輪流（10 ms 時間片）、PendSV 切換、1 ms tick、sleep／逾時
- 同步：wait queue、mutex、semaphore（可在中斷中 give）；SPI 匯流排與 CH376S 裝置鎖
- 系統呼叫：`svc` 進入，kernel 函式在呼叫者自己的 task context 執行（可阻塞）；使用者端介面在 `lib/sys.h`
- 記憶體：first-fit kmalloc（合併相鄰空間）；task 堆疊有紅區與 canary 檢查，溢位時 panic
- 當機紀錄：HardFault 與 panic 都存入 `.noinit`，重開後回報；開機印出重置原因
- 驅動：CH376S（USB 隨身碟 FAT）、W5500（硬體 TCP/IP）、CH9350（USB 鍵鼠，中斷接收）
- 系統 task：`input`（鍵鼠）、`diag`（TCP 5000 診斷服務）、`mon`（模組監控）、`idle`

| 板上測試（`make test`，15 項） | 結果 |
| --- | --- |
| W5500 送出／接收（SPI 25 MHz） | 1030 KiB/s ／ 871 KiB/s，逐 byte 驗證 |
| CH376S 寫入／讀取 256 KiB（SPI 12.5 MHz） | 120 kB/s ／ 231 kB/s，0 錯誤 |
| 搶佔：CPU 滿載時 10 ms ticker 最差延遲 | 0 ms |
| 同優先權輪流：兩個 CPU hog | 509／491 ms |
| 堆疊溢位偵測、HardFault 捕捉，重開後回報 | 通過 |
| 程式大小／靜態 RAM／heap | 16.7 KB ／ 3.5 KB ／ 11.6 KB（系統 task 建立後剩 8.5 KB） |

## 硬體

NU-LB_002 Rev 2.1（NUC140VE3CN），外接模組由 MB-102 5V 供電並與板子共地：

| 模組 | NUC140 腳位 |
| --- | --- |
| CP2102（log） | PB1 UART0 TX → RXD |
| CH9350（USB 鍵鼠，state 2、115200） | TX → PB4 UART1 RX；PB5／PB6／PB7 由韌體固定高，讓板載 LIN 收發器保持閒置 |
| CH376S（SPI，P_S 拔除、RD/WR 接地） | SCK PC9、MISO PC10、MOSI PC11、CS PC8、INT PB14 |
| W5500 | SCK PC9、MISO PC10、MOSI PC11、CS PB10、INT PB2、RST PB8 |

板子細節與限制（LIN 共用、CH376S 只能逐 byte 傳輸等）記錄在 [docs/PLAN.md](docs/PLAN.md) §2。

## 建置與使用

需要 `clang`、`ld.lld`、`llvm-objcopy`、Python 3，以及支援 Nu-Link 的 OpenOCD（0.12）。

```sh
make              # build/nnnix.elf、build/nnnix.bin
make host-test    # 在建置機器上跑單元測試
make flash        # 經 Nu-Link 寫入 APROM（只寫 APROM，不動 CONFIG／LDROM）
make test         # 燒錄 + 板上自動測試（需要 sudo：建立暫時的 network namespace）
python3 tools/console.py   # 看 kernel log（/dev/ttyUSB0，115200 8N1）
```

`make test` 需要一張 USB 網卡用網路線直接接 W5500。測試時會把它暫時移到隔離的 network namespace
（198.18.0.1/30，nnnix 是 198.18.0.2），結束後還原；網卡不能有 IPv4 位址，也不能是預設路由。
預設網卡 `eth1`，可用 `tools/nntest.py --iface` 指定。

OpenOCD 搜尋順序：`$NNNIX_OPENOCD_ROOT`（解開的 Debian openocd 套件根目錄）→ repo 上一層的
`.tools/openocd/root` → `PATH` 中的 `openocd`。

## 目錄

```
arch/nuc140/   啟動、向量表、時脈、腳位、context switch、HardFault、linker script
kernel/        排程器、同步、系統呼叫、kmalloc、klog、panic、模組監控、診斷服務
drivers/       uart spi ch376 w5500 ch9350 hidkey
lib/           printf 子集、字串函式、EABI 除法（M0 沒有除法指令）、系統呼叫介面 sys.h
user/          使用者程式（目前是排程示範）
tests/host/    主機單元測試（含實際錄下的 CH9350 資料）
tools/         flash.py、console.py、nntest.py
docs/          開發計畫
```

## 授權

GPL-3.0-or-later，見 [LICENSE](LICENSE)。
