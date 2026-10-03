# Nguồn và build

## Driver CAPT

- Source: https://github.com/ValdikSS/captdriver
- Commit: `892f320c019974a5b737e000fcc2c8582264a131`.
- Archive nguyên bản: `third_party/source/captdriver-892f320.tar.xz`; hash trong `sources.lock`.
- Source đang build: `third_party/captdriver/`, có chỉnh sửa cục bộ được mô tả trong README và patch `patches/captdriver-macos.patch`.
- License: GPL-3.0-or-later theo thông báo trong source; giữ file `COPYING` của upstream.

Ý tưởng cập nhật extended status và báo CUPS state được đối chiếu với các dự án:

- https://github.com/duy12i1i7/canon-LBP2900-for-macOS
- https://github.com/bechou0410/canon-lbp2900-macos27-driver

Các sửa đổi cục bộ áp dụng vào nhánh ValdikSS, không thay thế nó bằng trình tự protocol của nhánh cũ.

## Backend USB

- Source: https://github.com/OpenPrinting/cups
- Commit: `4bd119bbec57885993ab583c79ee5c70be77a7c2`, tag `v2.4.17`.
- Archive: `third_party/source/cups-4bd119b.tar.gz`; hash trong `sources.lock`.
- Build target: `backend/usb`, Darwin/IOKit, ARM64, deployment target macOS 26.0.
- Patch: `patches/cups-backend-name.patch` giới hạn discovery vào Canon LBP2900, dùng scheme `canon2900usb` và chọn USBGenericPrintingClass của Apple thay vì class driver của hãng còn sót trong IORegistry.
- Libcups tạm chỉ dùng khi build/link backend và được cấu hình không cần TLS; bản libcups này không được đóng gói. Backend USB cuối dùng libcups của macOS.
- Backend cuối liên kết `/usr/lib/libcups.2.dylib`, không kèm hoặc thay libcups hệ thống.
- License: Apache-2.0 với ngoại lệ CUPS; giữ LICENSE, NOTICE và thông báo riêng trong các file source.

Quy trình đóng gói ban đầu được tham khảo từ https://github.com/cvthien-dev/canon-lbp2900-macos; script của bản này dùng tên riêng và không cần Autoconf/Homebrew.

## Rebuild

`make release` dùng archive/source đã kèm, không tải mạng. Compiler, SDK và timestamp của công cụ Apple có thể làm binary/pkg thay hash giữa các máy build. `BUILD-MANIFEST.json` ghi host OS/build, compiler, deployment target và hash từng artifact.

Source tương ứng, local patches, giấy phép và các công cụ build/test/package được đóng vào archive phát hành và cũng cài kèm `.pkg` dưới tên `Corresponding-Source.tar.gz`.
