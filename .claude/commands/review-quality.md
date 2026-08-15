# Review Code Quality — SCA Project

Review đoạn code dưới đây theo tiêu chuẩn của SCA project. Kiểm tra toàn diện các vấn đề sau:

## Checklist Review

### FreeRTOS Safety
- [ ] Shared resource có mutex bảo vệ không?
- [ ] Error path có release mutex/semaphore không? (tránh deadlock)
- [ ] ISR có dùng `FromISR` variants không?
- [ ] Timeout hợp lý — không dùng `portMAX_DELAY` bừa bãi?

### Memory & Buffer Safety  
- [ ] Dùng `snprintf` thay vì `sprintf`?
- [ ] Array access có kiểm tra bounds không?
- [ ] Dynamic allocation (`malloc`) có được free không?

### Error Handling
- [ ] Return value của FreeRTOS API có được check không?
- [ ] Lỗi có được log bằng `ESP_LOGE` với TAG đúng không?
- [ ] Function có trả về error code rõ ràng không?

### Code Cleanliness
- [ ] Magic number? → Thay bằng `#define` constant
- [ ] Function có làm đúng 1 việc không? (Single Responsibility)
- [ ] Tên biến/function có self-explanatory không?
- [ ] Comment có giải thích "tại sao" chứ không chỉ "cái gì"?

### SCA-Specific
- [ ] Input từ BLE có được validate trước khi xử lý không?
- [ ] NVS read/write có ở đúng context (không phải ISR)?
- [ ] SPI CS pin cho MCP2515 vs DW3000 có được quản lý đúng không?
- [ ] HMAC verify có được thực hiện trước khi execute CAN command không?

## Code cần review:

$ARGUMENTS
