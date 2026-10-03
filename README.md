# Canon 2900 Driver - MacLife

Gói `.pkg` **thử nghiệm** cho Canon LBP2900 thường kết nối USB trực tiếp với Mac Apple Silicon (ARM64). Driver cộng đồng, không do Canon phát hành.

Gói dùng `ValdikSS/captdriver` và backend USB Darwin từ OpenPrinting CUPS. Không cần cài bộ Canon LBP3000/PhongBlack. Gói có filter, backend, PPD và queue riêng; không thay driver Canon cũ, không đổi máy in mặc định, không thay CUPS của Apple.

## Tải bộ cài

[**Tải Canon-LBP2900-ML-0.1.2.pkg**](https://github.com/maclifevn/Canon-2900-Driver-MacLife/releases/download/v0.1.2/Canon-LBP2900-ML-0.1.2.pkg)

Bản phát hành **v0.1.2** dành cho **Mac Apple Silicon chạy macOS 26 hoặc 27**, đã ký Developer ID, được Apple notarize (**Accepted**) và gắn ticket vào bộ cài. Kiểm tra Gatekeeper trả về **accepted / Notarized Developer ID**. Mở file `.pkg` và làm theo Installer; macOS sẽ hỏi quyền quản trị để cài driver.

Xem [hướng dẫn cài và gỡ](docs/INSTALL.md), [các bản phát hành](https://github.com/maclifevn/Canon-2900-Driver-MacLife/releases) và [cộng đồng MacLife và Đồng bọn](https://www.facebook.com/groups/maclife.vn). Release có source tương ứng và `SHA256SUMS` để đối chiếu file tải về.

## Phạm vi và trạng thái

- Mục tiêu: macOS 26 và 27, Apple Silicon, LBP2900 thường, USB trực tiếp.
- Deployment target của cả hai binary: macOS 26.0. Không dùng Rosetta.
- Bộ cài hiện tại không cài được trên macOS 15.x; installer chỉ nhận macOS 26/27 và binary yêu cầu tối thiểu macOS 26.0.
- Mặc định: A4, đen trắng, 600 dpi.
- Người dùng xác nhận bản 0.1.0 in thành công ngày 03/10/2026. Bản 0.1.1 giữ nguyên hai binary driver, đổi lời giới thiệu và tên máy in thành **Canon LBP2900**.
- Bản 0.1.2 thêm liên kết [cộng đồng MacLife và Đồng bọn](https://www.facebook.com/groups/maclife.vn) ở màn hình đầu installer; hai binary driver giữ nguyên.
- Chưa xác nhận riêng bản 0.1.2 trên máy in thật hoặc hoàn tất kiểm thử cả macOS 26 và 27.
- Chưa hỗ trợ/kiểm thử Intel, LBP2900+, print server hoặc nhiều LBP2900 cùng lúc.
- Bản phát hành v0.1.2 trên GitHub đã ký Developer ID và notarize. Tự build bằng `make release` tạo bản ký ad-hoc với installer chưa notarize; xem [quy trình notarize](docs/NOTARIZE.md) nếu có chứng chỉ và tài khoản Apple Developer riêng.

Các báo cáo in được của dự án nguồn là cơ sở phát triển, không phải chứng nhận tương thích cho bản này.

## Cài và gỡ

Xem [docs/INSTALL.md](docs/INSTALL.md). Installer cần quyền quản trị; không cần Homebrew hay công cụ build trên máy nhận gói.

Máy in được tạo: `Canon_LBP2900_OpenCAPT`, tên hiển thị **Canon LBP2900**. Cài đè bản 0.1.0 hoặc 0.1.1 sẽ cập nhật tên hiển thị của máy in hiện có.

## Build lại hoàn toàn từ source, không cần mạng

Yêu cầu Mac Apple Silicon và Xcode Command Line Tools, gồm `clang`, `make`, `python3`, CUPS tools và công cụ đóng gói của Apple.

```sh
make release
```

Gói và source tương ứng nằm trong `dist/`. Không chạy `make install`, không cài hoặc sửa driver hệ thống khi build/test.

`make build`: biên dịch filter, backend USB và PPD. `make package`: build và đóng gói. `make test`: thử compressor, lỗi input, bộ nhớ grouping và trạng thái driver bằng stub. `make verify`: kiểm tra gói đã build, chữ ký binary, deployment target, PPD, payload và checksum.

## Thành phần cài đặt

| Thành phần | Đường dẫn |
|---|---|
| Filter CAPT | `/usr/libexec/cups/filter/canon2900-rastertocapt` |
| Backend USB riêng | `/usr/libexec/cups/backend/canon2900usb` |
| PPD | `/Library/Printers/PPDs/Contents/Resources/Canon-LBP2900-OpenCAPT.ppd` |
| Hướng dẫn, giấy phép, source và công cụ gỡ | `/Library/Application Support/Canon LBP2900 OpenCAPT/` |
| Log installer | `/var/tmp/canon2900-opencapt-install.log` |

Không có kernel extension hay yêu cầu tắt SIP/Gatekeeper toàn hệ thống. Các URI của gói bắt đầu bằng `canon2900usb://`.

## Thay đổi so với upstream

- Luôn cập nhật extended status/page counters cho LBP2900.
- Báo `PAGE:` và trạng thái hết giấy/kẹt giấy/mở nắp qua CUPS khi driver nhận diện được.
- Giới hạn số lần poll các handshake và buffer drain; timeout trả lỗi thay vì báo in thành công.
- Input rỗng/sai raster trả lỗi; chỉ nhận raster đơn sắc 600 dpi của PPD.
- Tránh tràn buffer khi một band nén lớn hơn chunk limit của máy in.
- Backend riêng chỉ khám phá LBP2900, báo đúng scheme và dùng USB class của Apple để tránh phụ thuộc plugin Canon cũ.

Nhánh upstream vẫn giữ trình tự xử lý giấy/nút bấm. Khi hết giấy, có thể cần nạp giấy và nhấn nút trên máy in; khả năng tiếp tục job còn cần thử trên phần cứng.

Xem [SOURCE.md](SOURCE.md), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) và [docs/HARDWARE-TEST.md](docs/HARDWARE-TEST.md).
