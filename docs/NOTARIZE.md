# Ký và notarize bộ cài Canon LBP2900

Apple yêu cầu hai identity có private key trong Keychain: **Developer ID Application** để ký hai binary và **Developer ID Installer** để ký `.pkg`. Chứng chỉ Application không dùng thay Installer. Profile `notarytool` cần đăng nhập được vào dịch vụ Apple.

Quy trình chuẩn bị chữ ký và bộ cài diễn ra trong `build/notarize` và `dist/notarized`; các gói đã phát hành ở `dist` được giữ lại. Không biên dịch lại binary, không cài driver, không gửi job in và không thay đổi Gatekeeper/SIP.

```sh
chmod +x scripts/notarize.sh
./scripts/notarize.sh
```

Máy hiện dùng identity Application của team `CF5PGH3KGK` và profile `MNotch-notary`. Nếu dùng signing account khác, truyền tên đầy đủ bằng các biến sau:

```sh
DEVELOPER_ID_APPLICATION='Developer ID Application: Your Name (TEAMID)' \
DEVELOPER_ID_INSTALLER='Developer ID Installer: Your Name (TEAMID)' \
NOTARY_PROFILE='YourNotaryProfile' ./scripts/notarize.sh
```

Script ký từng binary với hardened runtime và timestamp, xác nhận vùng mã/dữ liệu giữ nguyên, thử startup không in, tạo/ký gói, kiểm tra payload, gửi Apple, xác nhận `Accepted`, staple ticket rồi kiểm tra `stapler validate` và Gatekeeper. Chỉ sau khi tất cả bước đạt mới có bản notarized hoàn tất. Log và checksum được lưu cùng gói.

## Cấp chứng chỉ Installer mới

Apple yêu cầu **Account Holder** tạo chứng chỉ Developer ID Installer. CSR của lần chuẩn bị này nằm tại `build/signing/Canon-LBP2900-Installer.certSigningRequest`. Private key tương ứng đã được import vào login Keychain, chỉ cho `productbuild` và `productsign` dùng; bản file trong `build/signing` có quyền `0600` và bị loại khỏi source/distribution. Chỉ upload CSR công khai lên Apple; không upload private key.

Trong Apple Developer → Certificates, tạo **Developer ID Installer**, chọn G2 Sub-CA nếu có lựa chọn, upload CSR, tải `.cer`. Import `.cer` vào login Keychain để ghép với private key đã import, rồi xác nhận identity Installer xuất hiện trong `security find-identity -v` trước khi chạy script. Không thu hồi các chứng chỉ đang sử dụng.

Nếu cần tạo profile mới, dùng prompt mật khẩu của `notarytool`, tránh đưa mật khẩu vào script hoặc lịch sử Terminal:

```sh
xcrun notarytool store-credentials 'Canon-LBP2900-notary' \
    --apple-id 'YOUR_APPLE_ID' --team-id 'YOUR_TEAM_ID'
```

Apple yêu cầu app-specific password cho cách đăng nhập này. Không cần gửi mật khẩu qua chat.

Tài liệu chính thức: [ký installer](https://help.apple.com/xcode/mac/current/en.lproj/deve51ce7c3d.html), [tạo Developer ID certificate](https://developer.apple.com/help/account/certificates/create-developer-id-certificates), [notarization workflow](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow).
