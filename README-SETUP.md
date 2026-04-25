# Hướng dẫn cài đặt vào SCA Project

## Bước 1: Copy CLAUDE.md vào root của project
```bash
cp CLAUDE.md ~/path/to/SCA-project/CLAUDE.md
```

## Bước 2: Copy thư mục commands
```bash
cp -r .claude ~/path/to/SCA-project/
```

## Cách dùng trong Claude Code

### Slash commands có sẵn:
| Command | Dùng khi nào |
|---------|-------------|
| `/review-quality <tên file>` | Muốn check code có clean/safe không |
| `/new-feature <mô tả>` | Bắt đầu implement tính năng mới |
| `/debug <mô tả lỗi>` | Đang bị bug không biết nguyên nhân |

### Ví dụ:
```
/review-quality friend_sharing/fs_key_manager.c
/new-feature Thêm chức năng revoke friend key qua BLE
/debug Task BLE bị crash sau 30 giây kết nối
```

## Cập nhật CLAUDE.md
Khi bạn thêm tính năng mới, hãy update CLAUDE.md:
- Thêm vào **File Structure** nếu tạo module mới
- Thêm vào **Các lỗi thường gặp** nếu phát hiện pattern bug mới
- Update **Friend Sharing** flow nếu design thay đổi
