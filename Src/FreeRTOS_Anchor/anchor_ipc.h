#pragma once
// anchor_ipc.h — Shared FreeRTOS IPC primitives, volatile state, and hardware handles.
// All other anchor_*.h modules include this file.

#include <BLEServer.h>
#include <BLECharacteristic.h>
#include <mcp2515.h>
#include "can_commands.h"
#include "friend_types.h"

// ── EventGroup bits ───────────────────────────────────────────────────────────
static EventGroupHandle_t sysEvents;
#define EVT_CONNECTED       (1 << 0)   // Tag connected BLE
#define EVT_AUTHED          (1 << 1)   // Tag HMAC auth OK
#define EVT_UWB_ACTIVE      (1 << 2)   // DW3000 ranging active
#define EVT_SIM_READY       (1 << 3)   // SIM module up, PDP active, time synced
#define EVT_SIM_INITIALIZED (1 << 4)   // simInit() succeeded at least once; never cleared

// ── BLE command queue ─────────────────────────────────────────────────────────
enum BleCmdType : uint8_t {
    BLE_KEY_CHECK,          // triggered on connect: check NVS key, fetch from server if absent
    BLE_SEND_CHALLENGE,
    BLE_AUTH_VERIFY,
    BLE_NOTIFY_UWB_ACTIVE,
    BLE_RESTART_ADV,
};
struct BleCmdMsg {
    BleCmdType type;
    uint8_t    data[32];
    uint8_t    dataLen;
};
static QueueHandle_t bleQueue;      // depth 8

// ── UWB queue ─────────────────────────────────────────────────────────────────
#define UWB_CMD_INIT   (0U)
#define UWB_CMD_DEINIT (1U)
static QueueHandle_t uwbQueue;      // depth 4

// ── CAN queue ─────────────────────────────────────────────────────────────────
#define CAN_CMD_LOCK   (0U)
#define CAN_CMD_UNLOCK (1U)
static QueueHandle_t canQueue;      // depth 4

// ── SPI mutex + bundle queue ──────────────────────────────────────────────────
static SemaphoreHandle_t spiMutex;
static QueueHandle_t     bundleQueue;  // depth 2

// ── SIM mutex + sleep state ───────────────────────────────────────────────────
// simMutex serialises all SIM AT-command sessions across tasks.
// s_simSleeping tracks whether the module is in AT+CFUN=0 (RF off) low-power mode.
static SemaphoreHandle_t simMutex;
static volatile bool     s_simSleeping = false;

// ── Volatile state ────────────────────────────────────────────────────────────
static volatile bool    carUnlocked            = false;
static volatile bool    hasKey                 = false;
static volatile bool    bleStarted             = false;
static volatile bool    deviceConnected        = false;
static volatile bool    authenticated          = false;
// Set by friendMgmtTask when bundle verified; allows TAG_UWB_READY from friend.
// Cleared on every onConnect.
static volatile bool    s_friendBundleVerified = false;
// Incremented on every onConnect — detects stale BLE_SEND_CHALLENGE messages.
static volatile uint8_t connectionGen          = 0;

// Active friend key for UWB STS (cleared on disconnect)
static uint8_t       s_activeFriendKey[16] = {};
static volatile bool s_uwbFriendMode       = false;

// ── BLE hardware handles ──────────────────────────────────────────────────────
static BLEServer         *pBleServer              = nullptr;
static BLECharacteristic *pCharacteristic          = nullptr;
static BLECharacteristic *pChallengeCharacteristic = nullptr;
static BLECharacteristic *pAuthCharacteristic      = nullptr;
static BLECharacteristic *pBundleSubmitChar        = nullptr;
static BLECharacteristic *pFriendStatusChar        = nullptr;

static uint8_t s_bundleBuf[BUNDLE_BIN_SIZE];
static size_t  s_bundleBufLen = 0;

// ── CAN hardware ──────────────────────────────────────────────────────────────
static MCP2515     *pMcp2515    = nullptr;
static CANCommands *pCanControl = nullptr;

// ── Auth ──────────────────────────────────────────────────────────────────────
static uint8_t currentChallenge[16];
static uint8_t pairingKey[16];
// Written by AuthChar callback (Core 0 BLE stack), read after copy into BleCmdMsg
static uint8_t responseBuffer[32];
static uint8_t responseBufferLen = 0;
