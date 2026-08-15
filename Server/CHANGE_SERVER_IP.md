# Thay đổi IP Server khi laptop đổi IP

Khi bạn chạy server trên laptop và địa chỉ IP LAN của laptop thay đổi, bạn cần cập nhật các cấu hình sau để app và Anchor vẫn kết nối đúng:

## 1. Tìm IP LAN mới của laptop
Trên Windows, mở PowerShell và chạy:
```powershell
ipconfig
```
Tìm mục `Wi-Fi` (hoặc `Ethernet`) và lấy `IPv4 Address`. Ví dụ: `10.0.7.178`.

## 2. Cập nhật IP trong Android app
Mở file:
- `AndroidApp/app/src/main/java/com/example/uwb/network/ApiClient.kt`

Tìm dòng:
```kotlin
.baseUrl("http://10.0.7.178:8000")
```
Đổi thành:
```kotlin
.baseUrl("http://<YOUR_NEW_IP>:8000")
```
Ví dụ:
```kotlin
.baseUrl("http://10.0.7.178:8000")
```

Sau đó rebuild app.

## 3. Cập nhật IP trong Anchor firmware
Mở file:
- `Src/FreeRTOS_Anchor/anchor_config.h`

Tìm dòng:
```c
#define SERVER_FALLBACK  "http://10.0.7.178:8000"
```
Đổi thành:
```c
#define SERVER_FALLBACK  "http://<YOUR_NEW_IP>:8000"
```
Ví dụ:
```c
#define SERVER_FALLBACK  "http://10.0.7.178:8000"
```

Sau đó build và flash lại Anchor.

## 4. (Nếu cần) Cập nhật URL server trên các client khác
Nếu bạn có mã khác gọi API server thẳng (ví dụ tập tin trong `Server/` hoặc phần test), cũng thay IP tương tự.

## 5. Kiểm tra lại
- Khởi động lại server và đảm bảo nó đang chạy.
- Từ máy khác cùng mạng, truy cập:
  `http://<YOUR_NEW_IP>:8000/health`
- Nếu trả về `200 OK`, tức là kết nối thành công.

## Ghi chú
- Luôn dùng `http://` chứ không phải `https://` nếu server chưa cài chứng chỉ TLS.
- Tránh dùng IP VPN (ví dụ `10.2.0.2`, `10.5.0.2`) — chỉ dùng IP LAN thật của Wi-Fi/Ethernet.
- Nếu laptop bật VPN, tốt nhất tắt VPN khi dùng server LAN.
