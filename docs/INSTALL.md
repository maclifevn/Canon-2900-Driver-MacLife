# Cài driver Canon LBP2900

Gói này dành cho **Mac Apple Silicon chạy macOS 26 hoặc 27** và **Canon LBP2900 thường**, nối USB trực tiếp. Người dùng đã xác nhận bản 0.1.0 in thành công; bản 0.1.2 giữ nguyên binary driver, cập nhật nội dung, tên hiển thị và liên kết nhóm MacLife.

Bộ cài hiện tại không cài được trên macOS 15.x: installer chỉ nhận macOS 26/27 và binary yêu cầu tối thiểu macOS 26.0.

## Cài đặt

1. Tải [`Canon-LBP2900-ML-0.1.2.pkg`](https://github.com/maclifevn/Canon-2900-Driver-MacLife/releases/download/v0.1.2/Canon-LBP2900-ML-0.1.2.pkg) từ [GitHub Release v0.1.2](https://github.com/maclifevn/Canon-2900-Driver-MacLife/releases/tag/v0.1.2).
2. Cắm USB, bật máy in và nạp giấy A4.
3. Mở `Canon-LBP2900-ML-0.1.2.pkg` và làm theo Installer. Có thể cài đè bản 0.1.0 hoặc 0.1.1; máy in hiện có sẽ được cập nhật tên.
4. Trong System Settings → Printers & Scanners, chọn **Canon LBP2900**.
5. In một trang PDF A4, rồi thử PDF hai trang. Giữ lại hàng đợi cũ để có thể quay lại nếu cần.

Bộ cài trên GitHub đã ký Developer ID, được Apple notarize và gắn ticket; có thể mở bình thường qua Installer. macOS vẫn hỏi mật khẩu quản trị khi cài driver. SHA-256 của bộ cài:

```text
98dd9e6eb1bee8dbd6c181ab2b25e00c9fc90226ee5f7f797dc715d82421c175
```

Để đối chiếu, chạy `shasum -a 256 Canon-LBP2900-ML-0.1.2.pkg` từ thư mục chứa file tải về. Release cũng có `SHA256SUMS` cho các file đính kèm.

Bản tự build bằng `make release` trong `dist` chưa ký Developer ID/notarize và giữ tên `Canon-LBP2900-OpenCAPT-macOS26-27-arm64-0.1.2.pkg`. Nếu macOS chặn gói tự build, dùng System Settings → Privacy & Security → Open Anyway cho đúng gói đã kiểm tra. Nếu không có lựa chọn đó, có thể cài qua Terminal, từ thư mục chứa file:

```sh
sudo /usr/sbin/installer -pkg './Canon-LBP2900-OpenCAPT-macOS26-27-arm64-0.1.2.pkg' -target /
```

Không cần tắt Gatekeeper/SIP toàn hệ thống hoặc cài bộ Canon LBP3000 trước.

## Kiểm tra lỗi

```sh
lpstat -p Canon_LBP2900_OpenCAPT -v Canon_LBP2900_OpenCAPT
```

URI đúng bắt đầu bằng `canon2900usb://Canon/LBP2900`. Nếu máy chờ kết nối, kiểm tra dây USB và nguồn máy in. Khi hết giấy, nạp giấy rồi nhấn nút trên máy in. Nếu job lỗi làm queue dừng, xử lý lỗi và nhấn Resume trong Print Center; có thể cần gửi lại job.

Log cài đặt: `/var/tmp/canon2900-opencapt-install.log`. Log CUPS nếu có: `/var/log/cups/error_log`. Ghi lại macOS/build, loại chip và việc máy có ra giấy thật hay chỉ báo completed.

## Gỡ gói này

Nếu đã tải `uninstall.sh` từ release hoặc đang ở thư mục `dist` của bản tự build:

```sh
sudo /bin/sh ./uninstall.sh
```

Hoặc dùng bản đã cài:

```sh
sudo /bin/sh '/Library/Application Support/Canon LBP2900 OpenCAPT/uninstall.sh'
```

Script kiểm tra receipt và dấu nhận diện của queue, rồi gỡ queue/file của gói này. Không gỡ bộ Canon/PhongBlack hoặc các máy in khác.
