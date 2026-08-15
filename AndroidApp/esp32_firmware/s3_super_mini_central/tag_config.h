#pragma once

// =============================================================================
// BLE UUIDs — phải khớp với Anchor firmware
// =============================================================================
#define SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"
#define CHARACTERISTIC_UUID "abcdef12-3456-7890-abcd-ef1234567890"
#define AUTH_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHALLENGE_CHAR_UUID "ceb5483e-36e1-4688-b7f5-ea07361b26a9"
// Nếu Anchor chưa có HMAC (DevKit cũ): chỉ cần SERVICE_UUID và CHARACTERISTIC_UUID khớp.
// Tag sẽ tự bỏ qua bước auth nếu không tìm thấy AUTH_CHAR và CHALLENGE_CHAR.

// =============================================================================
// Distance thresholds
// =============================================================================
#define UWB_UNLOCK_DISTANCE_M   (10.0f)   // <= threshold → UNLOCK
#define UWB_LOCK_DISTANCE_M     (10.5f)   // >  threshold → LOCK (0.5m hysteresis)
#define UWB_FAR_DISTANCE_M      (10.0f)  // >  threshold → dừng UWB, dùng RSSI

// =============================================================================
// RSSI thresholds
// =============================================================================
#define RSSI_THRESHOLD_DBM      (-200)   // RSSI trên ngưỡng → BLE đủ gần để kết nối
#define RSSI_CHECK_INTERVAL_MS  (1000U)  // chu kỳ check RSSI khi UWB đang dừng

// =============================================================================
// UWB retry
// =============================================================================
#define UWB_REQUEST_RETRY_MS    (5000U)  // retry TAG_UWB_READY nếu Anchor chưa phản hồi

// =============================================================================
// Hardware pins (DW3000 — chỉ dùng khi UWB_ENABLED = 1)
// =============================================================================
#define PIN_RST (5)
#define PIN_IRQ (4)
#define PIN_SS  (10)

// =============================================================================
// UWB frame constants
// =============================================================================
#define TX_ANT_DLY              (16385U)
#define RX_ANT_DLY              (16385U)
#define ALL_MSG_COMMON_LEN      (10U)
#define ALL_MSG_SN_IDX          (2U)
#define RESP_MSG_POLL_RX_TS_IDX (10U)
#define RESP_MSG_RESP_TX_TS_IDX (14U)
#define RESP_MSG_TS_LEN         (4U)
#define POLL_TX_TO_RESP_RX_DLY_UUS (500U)
#define RESP_RX_TIMEOUT_UUS     (50000U)
#define MSG_BUFFER_SIZE         (20U)

// =============================================================================
// Distance filter
// =============================================================================
#define DIST_FILTER_SIZE (5)

// =============================================================================
// Feature flags
// =============================================================================
#define UWB_ENABLED 1   // set to 0 to run BLE-only (no DW3000 hardware needed)

// =============================================================================
// Friend Sharing — BLE UUIDs (phải khớp với Anchor firmware anchor_config.h)
// =============================================================================
#define FRIEND_SERVICE_UUID     "0000FACE-0000-1000-8000-00805F9B34FB"
#define FRIEND_BUNDLE_CHAR_UUID "0000FA01-0000-1000-8000-00805F9B34FB"
#define FRIEND_STATUS_CHAR_UUID "0000FA03-0000-1000-8000-00805F9B34FB"

// =============================================================================
// Friend Sharing — wire format constants (phải khớp với friend_types.h của Anchor)
// =============================================================================
#define FRIEND_ID_LEN           8     // bytes (16 hex chars)
#define FRIEND_KEY_LEN          16    // bytes
#define BUNDLE_BIN_SIZE         106   // fixed 106-byte binary bundle
#define BUNDLE_SIGNED_PART      42    // signed_part = bytes[0..41]
#define BUNDLE_SIG_SIZE         64    // raw r||s = bytes[42..105]
