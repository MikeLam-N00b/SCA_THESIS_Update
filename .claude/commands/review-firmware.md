# Review Firmware — SCA Project

Bạn là senior embedded systems engineer + security specialist. Review đoạn code firmware C/Arduino dưới đây theo tiêu chuẩn của SCA project.

## Context
- MCU: ESP32-S3, FreeRTOS với 3 tasks (bleTask Core0/P3, uwbTask Core1/P4, canTask Core1/P2)
- DW3000 và MCP2515 share SPI2 bus → spiMutex bắt buộc
- mbedTLS cho HMAC-SHA256, ECDH P-256, AES-128-GCM, HKDF

## Checklist Review

### FreeRTOS Safety (embedded-systems skill)
- [ ] Shared resource có `spiMutex` bảo vệ không?
- [ ] BLE callback chỉ `xQueueSend` — không xử lý logic?
- [ ] `volatile` cho biến shared giữa ISR và task?
- [ ] Error path có `xSemaphoreGive` không? (tránh deadlock)
- [ ] ISR chỉ set flag / send queue — không blocking?
- [ ] `portYIELD_FROM_ISR(xHigherPriorityTaskWoken)` sau FromISR API?
- [ ] Cross-core BLE call từ Core 1 → dùng `bleWriteQueue`?
- [ ] `connectionGen` kiểm tra cho BLE_SEND_CHALLENGE?

### Memory & Buffer Safety
- [ ] `snprintf` thay `sprintf`?
- [ ] `getData()/getLength()` thay `getValue()` trong BLE callbacks?
- [ ] `memcmp` thay `String.startsWith()` trong callbacks?
- [ ] `float` thay `double` cho timing/distance calc?
- [ ] Array access có bounds check?

### UWB / SPI
- [ ] `dwt_configurestsloadiv()` trước mỗi RX cycle?
- [ ] `dwt_readstsquality()` check trước khi accept frame?
- [ ] `dwt_forcetrxoff()` + `digitalWrite(PIN_SS, HIGH)` trước CAN op?
- [ ] `spiMutex` timeout handled gracefully?

### Security (secure-code-guardian skill)
- [ ] HMAC verify dùng `mbedtls_md_hmac()` — không tự implement?
- [ ] Input từ BLE validate length trước `memcpy`?
- [ ] NVS key string ≤ 15 chars?
- [ ] STS quality reject = chống relay attack?

### Code Quality
- [ ] Magic number → `#define` constant?
- [ ] Error có log rõ ràng `Serial.printf("[MODULE] LOI: ...")`?
- [ ] Không có `portMAX_DELAY` không có lý do?
- [ ] Stack size có comment giải thích?

## Code cần review:
$ARGUMENTS
