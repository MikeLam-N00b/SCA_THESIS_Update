#pragma once

// =============================================================================
// USER CONFIGURATION — edit before flashing
// =============================================================================

// ── SIM Module key provisioning ───────────────────────────────────────────────
#define SIM_RX_PIN       (8)
#define SIM_TX_PIN       (6)
#define SIM_BAUD         (115200)
#define SIM_APN          "v-internet"   // Viettel; Mobifone: "m-wap"; Vinaphone: "m3-world"
#define VEHICLE_ID       "1HGBH41JXMN109186"
#define SERVER_FALLBACK  "http://139.59.232.153:8000"

// ── BLE ───────────────────────────────────────────────────────────────────────
#define DEVICE_NAME         "SmartCar_Vehicle"
#define SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"
#define CHARACTERISTIC_UUID "abcdef12-3456-7890-abcd-ef1234567890"
#define AUTH_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHALLENGE_CHAR_UUID "ceb5483e-36e1-4688-b7f5-ea07361b26a9"

// Delay before sending challenge (gives Tag time to subscribe to notifications)
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
