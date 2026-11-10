# Cài driver Canon LBP2900

Gói v0.1.3 dành cho **Mac Apple Silicon chạy macOS 26 hoặc 27** và **Canon LBP2900 thường**, nối USB trực tiếp. Bản này giữ phiên USB giữa các lệnh in và đã qua kiểm thử với máy in mô phỏng trên macOS 27.0.1.

Bộ cài hiện tại không cài được trên macOS 15.x: installer chỉ nhận macOS 26/27 và binary yêu cầu tối thiểu macOS 26.0.

## Cài đặt

1. Tải bộ cài [`Canon-LBP2900-ML-0.1.3.pkg`](https://github.com/maclifevn/Canon-2900-Driver-MacLife/releases/download/v0.1.3/Canon-LBP2900-ML-0.1.3.pkg).
2. Cắm USB, bật máy in và nạp giấy A4.
3. Chờ các lệnh đang in hoàn tất, rồi mở `Canon-LBP2900-ML-0.1.3.pkg` và làm theo Installer. Có thể cài đè v0.1.0 đến v0.1.2.
4. Trong System Settings → Printers & Scanners, chọn **Canon LBP2900**.
5. In một trang PDF A4, rồi thử PDF hai trang. Giữ lại hàng đợi cũ để có thể quay lại nếu cần.

Bộ cài trên GitHub đã ký Developer ID, được Apple notarize và gắn ticket. macOS hỏi mật khẩu quản trị khi cài driver. Để đối chiếu checksum, chạy `shasum -a 256 Canon-LBP2900-ML-0.1.3.pkg` từ thư mục chứa file tải về và so với [SHA256SUMS của bản phát hành](https://github.com/maclifevn/Canon-2900-Driver-MacLife/releases/download/v0.1.3/SHA256SUMS).

Bản thử nghiệm v0.1.3 tự build bằng `make release` nằm trong `dist` với tên `Canon-LBP2900-OpenCAPT-macOS26-27-arm64-0.1.3.pkg`. Chờ hàng đợi in xong rồi cài đè lên bản cũ. Bản tự build chưa ký Developer ID và chưa notarize. Có thể cài qua Terminal từ thư mục chứa file.

```sh
sudo /usr/sbin/installer -pkg './Canon-LBP2900-OpenCAPT-macOS26-27-arm64-0.1.3.pkg' -target /
```

Không cần tắt Gatekeeper/SIP toàn hệ thống hoặc cài bộ Canon LBP3000 trước.

## Kiểm tra lỗi

```sh
lpstat -p Canon_LBP2900_OpenCAPT -v Canon_LBP2900_OpenCAPT
```

URI đúng bắt đầu bằng `canon2900usb://Canon/LBP2900`. Nếu máy chờ kết nối, kiểm tra dây USB và nguồn máy in. Khi hết giấy, nạp giấy rồi nhấn nút trên máy in. Nếu job lỗi làm queue dừng, xử lý lỗi và nhấn Resume trong Print Center; có thể cần gửi lại job.

Log cài đặt: `/var/tmp/canon2900-opencapt-install.log`. Log CUPS nếu có: `/var/log/cups/error_log`. Ghi lại macOS/build, loại chip và việc máy có ra giấy thật hay chỉ báo completed.

Với v0.1.3, thử gửi 10 lệnh in một trang riêng biệt sau khi bật máy in. Nếu lệnh sau dừng, ghi lại số thứ tự lệnh và thông báo trong Print Center. Có thể bật log CUPS, tái hiện lỗi rồi lấy các dòng CAPT cuối cùng.

```sh
sudo cupsctl --debug-logging
# Tái hiện lỗi trước khi lấy log.
sudo tail -n 300 /var/log/cups/error_log | grep 'CAPT:' | tail -n 30
sudo cupsctl --no-debug-logging
```

Dịch vụ `org.opencapt.lbp2900.usb` giữ cổng USB sau khi in. Dùng hàng đợi Canon LBP2900 của gói này cho máy in đã kết nối. Nếu cần quay về driver khác cho cùng máy in, gỡ v0.1.3 để giải phóng phiên USB.

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
