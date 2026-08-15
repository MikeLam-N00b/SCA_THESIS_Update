# Implement New Feature — SCA Project

Implement tính năng sau cho SCA project theo đúng convention của project.

## Yêu cầu bắt buộc khi viết code mới

### Structure
- Tạo module riêng trong thư mục phù hợp (`ble/`, `can/`, `security/`, `friend_sharing/`...)
- Header file có include guard `#pragma once`
- Mỗi module có `TAG` riêng cho logging

### FreeRTOS Pattern chuẩn
```c
// Tạo task
xTaskCreate(task_fn, "task_name", STACK_SIZE, param, PRIORITY, &handle);

// Mutex pattern an toàn
if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    // critical section
    xSemaphoreGive(mutex);
} else {
    ESP_LOGE(TAG, "Failed to acquire mutex");
    return ESP_ERR_TIMEOUT;
}
```

### Error handling pattern
```c
esp_err_t ret = some_function();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "some_function failed: %s", esp_err_to_name(ret));
    return ret;
}
```

### Friend Sharing context
Nếu feature liên quan đến Friend Sharing:
- Key nhận từ LTE phải được validate HMAC trước khi lưu NVS
- NVS key format: `fs_key_{device_id}` với TTL field
- Sau khi verify xong mới gửi CAN command

## Tính năng cần implement:

$ARGUMENTS
