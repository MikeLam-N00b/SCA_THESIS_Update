#pragma once

// =============================================================================
// USER CONFIGURATION — edit before flashing
// =============================================================================

// ── WiFi key provisioning ─────────────────────────────────────────────────────
#define WIFI_SSID        "CISAT 5G"
#define WIFI_PASSWORD    "09091981"             // điền mật khẩu nếu có

// ── SIM Module key provisioning (dự phòng) ───────────────────────────────────
#define SIM_RX_PIN       (8)
#define SIM_TX_PIN       (6)
#define SIM_BAUD         (115200)
#define SIM_APN          "v-internet"   // Viettel; Mobifone: "m-wap"; Vinaphone: "m3-world"

#define VEHICLE_ID       "1HGBH41JXMN109186"
#define SERVER_FALLBACK  "http://10.0.7.178:8000"
// ── BLE ───────────────────────────────────────────────────────────────────────
#define DEVICE_NAME         "SmartCar_Vehicle"
#define SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"
#define CHARACTERISTIC_UUID "abcdef12-3456-7890-abcd-ef1234567890"
#define AUTH_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHALLENGE_CHAR_UUID "ceb5483e-36e1-4688-b7f5-ea07361b26a9"

// Delay before sending challenge (gives Tag time to subscribe to notifications).
// Android BLE stack + S3 service discovery takes ~1.2 s in practice;
// 2000 ms gives comfortable margin so the notify arrives after CCCD is written.
#define CHALLENGE_SEND_DELAY_MS (2000U)

// ── Hardware pins ─────────────────────────────────────────────────────────────
#define PIN_RST  (5)
#define PIN_IRQ  (4)
#define PIN_SS   (10)
#define CAN_CS   (9)
#define PIN_SCK  (12)
#define PIN_MISO (13)
#define PIN_MOSI (11)
#define MCP_CLOCK MCP_8MHZ


// ── UWB frame constants ───────────────────────────────────────────────────────
#define TX_ANT_DLY              (16385U)
#define RX_ANT_DLY              (16385U)
#define ALL_MSG_COMMON_LEN      (10U)
#define ALL_MSG_SN_IDX          (2U)
#define RESP_MSG_POLL_RX_TS_IDX (10U)
#define RESP_MSG_RESP_TX_TS_IDX (14U)
#define MSG_BUFFER_SIZE         (20U)
// Delay Poll RMARKER → Response RMARKER.
// Với FreeRTOS, uwbTask pin cứng Core 1 priority 4 — BLE không còn preempt Core 1.
// PLEN_1024: preamble ~1050µs + frame ~350µs = RXFCG at ~1400µs after POLL RMARKER.
// 3000µs gives ~1600µs processing margin — sufficient for FreeRTOS Core 1 priority 4.
#define POLL_RX_TO_RESP_TX_DLY_UUS (3000U)

// Speed of light và DWT time units
#define SPEED_OF_LIGHT  299702547.0
#define UUS_TO_DWT_TIME 63898

// ── FreeRTOS task config ──────────────────────────────────────────────────────
#define BLE_TASK_STACK   (10240)  // lớn hơn: chứa HMAC verify + BLE stack
#define UWB_TASK_STACK   (8192)
#define CAN_TASK_STACK   (4096)
#define BLE_TASK_PRIO    (3)
#define UWB_TASK_PRIO    (4)      // cao nhất → DW3000 không bị preempt
#define CAN_TASK_PRIO    (2)
#define BLE_TASK_CORE    (0)
#define UWB_TASK_CORE    (1)
#define CAN_TASK_CORE    (1)

// ── Friend Sharing task config ────────────────────────────────────────────────
// friendMgmtTask: Core 0 (BLE stack lives here); 16 KB for mbedtls pk_verify
//   + HTTPClient + ArduinoJson.
// revocationSyncTask: Core 0, lowest priority; sleeps 5 min between syncs.
#define FRIEND_TASK_STACK    (16384)
#define FRIEND_TASK_PRIO     (2)
#define FRIEND_TASK_CORE     (0)
#define REVSYNC_TASK_STACK   (8192)
#define REVSYNC_TASK_PRIO    (1)
#define REVSYNC_TASK_CORE    (0)

// ── Friend Sharing BLE UUIDs ──────────────────────────────────────────────────
// 16-bit UUIDs 0xFACE / 0xFA01 / 0xFA03 expanded to 128-bit Bluetooth base UUID.
#define FRIEND_SERVICE_UUID     "0000FACE-0000-1000-8000-00805F9B34FB"
#define FRIEND_BUNDLE_CHAR_UUID "0000FA01-0000-1000-8000-00805F9B34FB"
#define FRIEND_STATUS_CHAR_UUID "0000FA03-0000-1000-8000-00805F9B34FB"
