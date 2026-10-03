# Xác nhận trên máy in thật

Các kiểm tra bằng stub/compressor và kiểm tra `.pkg` không thay thế việc xác nhận giấy ra đúng trên máy thật.

Ngày 03/10/2026, người dùng xác nhận bản 0.1.0 đã in thành công. Bản 0.1.1 chỉ cập nhật nội dung installer và tên máy in, dùng lại hai binary của bản 0.1.0. Báo cáo này không thay thế toàn bộ các tình huống trong bảng và danh sách dưới đây.

| Môi trường | Trạng thái |
|---|---|
| macOS 27.0 ARM64: build và kiểm tra tự động | Xem `dist/VALIDATION.txt` sau `make release` |
| macOS 26 ARM64: cài và in vật lý bản này | Chưa kiểm thử |
| macOS 27 ARM64: cài và in vật lý bản này | Chưa kiểm thử |
| Intel, LBP2900+, kết nối mạng | Ngoài phạm vi bản thử |

Với mỗi phiên bản macOS 26 và 27, ghi model Mac/chip, version/build OS, USB ID của máy in và checksum gói. Thực hiện:

1. Cài khi máy in đã cắm; xác nhận queue tồn tại sau Installer và sau khởi động lại.
2. Cài khi máy in chưa cắm; cắm lại và thử in.
3. In PDF A4 1, 2 và 10 trang; kiểm tra số trang, nội dung, lề và trang cuối thực sự ra giấy.
4. In trang có chữ/ảnh dày, hai bản sao và nhiều job liên tiếp; kiểm tra không lệch/trùng/mất trang.
5. Hủy job đang in; rút/cắm USB; tắt/bật máy in và gửi job tiếp theo.
6. Hết giấy giữa job: kiểm tra thông báo, nạp giấy, nhấn nút máy in và Resume nếu queue dừng. Ghi rõ có cần gửi lại job và có trùng trang không.
7. Mở nắp/kẹt giấy: kiểm tra báo lỗi, phục hồi sau xử lý.
8. Cài cùng bộ Canon cũ; xác nhận queue cũ và máy in mặc định không bị đổi. Không gửi job từ cả hai queue vào cùng một máy in đồng thời.
9. Cài đè bản này rồi gỡ; xác nhận chỉ file/queue OpenCAPT được xử lý.

Chỉ đánh dấu đạt sau khi người thử nhìn thấy và kiểm tra bản in. CUPS báo `completed` chưa đủ.
