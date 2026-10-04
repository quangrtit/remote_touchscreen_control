# Remote Touchscreen Control

Biến điện thoại hoặc máy tính bảng thành màn hình cảm ứng từ xa cho Windows trong
cùng mạng LAN. Ứng dụng truyền hình ảnh desktop độ trễ thấp tới trình duyệt và
chuyển Pointer Events thành Windows Touch Injection, phù hợp để điều khiển desktop,
ứng dụng cảm ứng và game Android chạy trong BlueStacks.

## Tính năng

- Capture màn hình chính bằng DXGI Desktop Duplication.
- Encode H.264 low-latency bằng Windows Media Foundation.
- Truyền H.264 Annex B qua WebSocket nhị phân; không gửi chuỗi PNG/JPEG.
- Safari, Chrome và Edge ưu tiên WebCodecs để giải mã từng frame trực tiếp.
- Tự fallback sang fragmented MP4 + Media Source khi WebCodecs không ổn định.
- Preset realtime mặc định: 1280×720, 60 FPS, 8 Mbps.
- Multi-touch nguyên tử tối đa 10 contact: có thể giữ joystick và bấm kỹ năng đồng
  thời bằng hai tay.
- Touch heartbeat 20 Hz giữ contact lâu không bị Windows tự hủy.
- Thu phóng cục bộ 1×–4×, pan bằng một ngón và pinch bằng hai ngón.
- Hai chế độ hiển thị: `Vừa màn hình` và `Lấp đầy`.
- Tự reconnect input/video và gửi keyframe mới ngay khi client kết nối lại.
- Session token, giới hạn client IPv4 private/LAN và chặn PC local chiếm phiên của
  điện thoại.
- Log chẩn đoán từ trình duyệt được chuyển về console host.

## Kiến trúc

```text
Windows desktop
    │
    ├─ DXGI Desktop Duplication (BGRA)
    │       └─ scale + BGRA → NV12 trong một lượt
    │               └─ Media Foundation H.264 low-latency
    │                       └─ binary WebSocket /screen
    │                               └─ WebCodecs / Media Source
    │                                       └─ Canvas/Video trên điện thoại
    │
Điện thoại Pointer Events
    └─ binary WebSocket /input
            └─ multi-touch state tracker
                    └─ InjectTouchInput
                            └─ Windows / BlueStacks
```

Capture, encoder, streaming, network, protocol và input được tách thành các module
độc lập trong `host/src`. Web client nằm trong `web/` và được copy cạnh executable
sau khi build.

## Yêu cầu

- Windows 10 hoặc Windows 11.
- GPU/driver hỗ trợ DXGI Desktop Duplication.
- CMake 3.24 trở lên.
- Compiler hỗ trợ C++20:
  - Visual Studio 2022 Build Tools; hoặc
  - MinGW-w64 GCC phiên bản mới.
- Ninja được khuyến nghị để build nhanh.
- Điện thoại/máy tính bảng và PC kết nối cùng mạng LAN/Wi-Fi.
- Safari 16.4+, Chrome hoặc Edge phiên bản có WebCodecs được khuyến nghị.

## Build

Mở PowerShell tại thư mục repository:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Executable được tạo tại:

```text
build/host/RemoteTouchHost.exe
```

Kiểm tra thủ công pipeline DXGI → scale → H.264:

```powershell
.\build\host\video_pipeline_smoke.exe 60 8000000 1280 180
```

Kết quả hợp lệ sẽ có `first-key=1` và `sps=1`.

## Chạy nhanh

Preset khuyến nghị để chơi game qua Wi-Fi 5 GHz:

```powershell
.\build\host\RemoteTouchHost.exe `
  --fps 60 `
  --bitrate 8000000 `
  --max-width 1280
```

Host sẽ tạo session token ngẫu nhiên và in URL, ví dụ:

```text
http://192.168.1.73:8080/?token=<session-token>
```

Mở đúng URL vừa được in trên điện thoại hoặc máy tính bảng. Khi Windows Firewall
hỏi quyền, chỉ cho phép `RemoteTouchHost.exe` trên profile `Private`.

Giữ cửa sổ PowerShell đang chạy. Nhấn `Ctrl+C` để dừng host.

### Token cố định khi phát triển

Để không phải nhập URL mới sau mỗi lần restart:

```powershell
.\build\host\RemoteTouchHost.exe `
  --fps 60 `
  --bitrate 8000000 `
  --max-width 1280 `
  --session-token RemoteTouchLocal2026
```

URL tương ứng:

```text
http://<IP-PC>:8080/?token=RemoteTouchLocal2026
```

Token tự đặt phải dài 16–128 ký tự và chỉ chứa chữ, số, `-` hoặc `_`.

## Tùy chọn dòng lệnh

```text
RemoteTouchHost [--port 8080]
                [--bind 0.0.0.0]
                [--web-root PATH]
                [--fps 60]
                [--bitrate 8000000]
                [--max-width 1280]
                [--session-token TOKEN]
                [--verbose-input]
```

| Tùy chọn | Mặc định | Ý nghĩa |
|---|---:|---|
| `--port` | `8080` | Cổng HTTP và WebSocket. |
| `--bind` | `0.0.0.0` | Địa chỉ IPv4 host lắng nghe. |
| `--web-root` | cạnh executable | Thư mục chứa web client. |
| `--fps` | `60` | FPS mục tiêu, hợp lệ từ 10 đến 60. |
| `--bitrate` | `8000000` | Bitrate H.264, đơn vị bit/giây. |
| `--max-width` | `1280` | Giới hạn chiều rộng video; `0` giữ độ phân giải native. |
| `--session-token` | ngẫu nhiên | Token ổn định dành cho phát triển/test. |
| `--verbose-input` | tắt | In cả sự kiện `MOVE` để chẩn đoán touch. |

## Sử dụng trên điện thoại/iPad

1. Mở URL có token do host in ra.
2. Xoay ngang thiết bị để có vùng điều khiển lớn nhất.
3. Chọn `Vừa màn hình` để thấy toàn bộ desktop.
4. Chọn `Lấp đầy` để phủ kín viewport; một phần hình ảnh có thể bị cắt.
5. Với iPhone/iPad, có thể dùng `Share → Add to Home Screen` để giảm giao diện
   trình duyệt.

### Multi-touch khi chơi game

Ở chế độ bình thường, toàn bộ pointer được gửi sang Windows. Console host sẽ hiển
thị số contact đang hoạt động:

```text
DOWN ... active=1
DOWN ... active=2
UP   ... active=1
UP   ... active=0
```

Nếu BlueStacks chạy với quyền Administrator, nên chạy host với cùng mức quyền để
tránh giới hạn input giữa các integrity level.

### Thu phóng chi tiết nhỏ

1. Nhấn `Thu phóng`.
2. Pinch hai ngón để zoom từ 1× đến 4×.
3. Kéo một ngón để di chuyển vùng đang xem.
4. Chạm nhanh để gửi một touch tới đúng tọa độ Windows đang hiển thị.
5. Pinch về 100% hoặc chọn lại chế độ hiển thị để reset.
6. Nhấn `Xong` để trả toàn bộ pointer về chế độ multi-touch chơi game.

Chế độ zoom được tách riêng để thao tác giữ joystick + bấm kỹ năng không bị hiểu
nhầm thành pinch.

## Preset chất lượng

### Realtime cân bằng — khuyến nghị

```powershell
.\build\host\RemoteTouchHost.exe --fps 60 --bitrate 8000000 --max-width 1280
```

### Wi-Fi yếu

```powershell
.\build\host\RemoteTouchHost.exe --fps 30 --bitrate 4000000 --max-width 1280
```

### Độ phân giải desktop nguyên bản

```powershell
.\build\host\RemoteTouchHost.exe --fps 60 --bitrate 12000000 --max-width 0
```

Native 1080p cần nhiều CPU, bitrate và năng lực decode hơn. Với màn hình điện thoại,
720p60 thường cho cảm giác điều khiển tốt hơn 1080p30.

## Khắc phục sự cố

### Trang đứng ở “Đang kết nối”

- Đóng hẳn tab cũ và mở lại đúng URL/token vừa được host in.
- Kiểm tra điện thoại và PC đang ở cùng Wi-Fi/LAN.
- Dùng IP Wi-Fi/Ethernet thật, không dùng địa chỉ VMware, VPN hoặc WARP.
- Cho phép executable qua Windows Firewall profile `Private`.
- Kiểm tra cổng 8080 không bị ứng dụng khác sử dụng.

```powershell
Get-NetTCPConnection -LocalPort 8080 -State Listen
```

### Có touch nhưng không có hình

- Refresh trang để tải web client mới nhất.
- Quan sát log `screen-first-packet`, `webcodecs-rendered` hoặc `mse-rendered` trên
  console host.
- Buộc chế độ Media Source để kiểm tra tương thích:

```text
http://<IP-PC>:8080/?token=<TOKEN>&decoder=mse
```

### Hình ảnh trễ dần

- Không mở web client trên chính màn hình đang capture vì sẽ tạo hiệu ứng gương vô
  hạn và khiến toàn bộ desktop thay đổi mỗi frame.
- Tắt chế độ tiết kiệm pin trên điện thoại.
- Dùng Wi-Fi 5 GHz và giảm bitrate/FPS nếu tín hiệu yếu.
- Giữ `--max-width 1280`; chỉ dùng native khi thực sự cần.

### BlueStacks chỉ nhận một ngón

- Kiểm tra log có đạt `active=2` hay không.
- Chắc chắn nút `Thu phóng` đang ở trạng thái tắt/`Xong`.
- Kiểm tra cấu hình game controls của BlueStacks.
- Chạy BlueStacks và host cùng mức quyền.

## Hiệu năng và transport

Session token chỉ được kiểm tra khi mở HTTP/WebSocket, không nằm trong vòng xử lý
từng frame. Stream LAN sử dụng `ws://` nhị phân, không TLS và không bật nén
WebSocket. Vì vậy bỏ xác thực không tạo khác biệt latency đáng kể nhưng sẽ cho phép
thiết bị bất kỳ trong LAN điều khiển PC.

H.264 vẫn cần thiết ngay cả trong LAN: raw NV12 1920×1080 ở 60 FPS xấp xỉ
187 MB/s, còn BGRA gần 498 MB/s. Preset 1280×720 giảm hơn một nửa số pixel phải
chuyển màu, encode, truyền và decode.

## Kiểm thử

Các test hiện có:

- `touch_packet_tests`: kiểm tra binary touch protocol và validation.
- `touch_contact_tracker_tests`: kiểm tra frame hai ngón giữ joystick/bấm kỹ năng.
- `video_pipeline_smoke`: kiểm tra capture, scale, encoder, keyframe và SPS.

Chạy lại toàn bộ test:

```powershell
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Bảo mật

- Chỉ sử dụng trong mạng LAN tin cậy.
- Server chỉ chấp nhận địa chỉ IPv4 private hoặc loopback.
- Không port-forward cổng 8080 trực tiếp ra Internet.
- Không chia sẻ URL chứa session token.
- Nên để host tự tạo token mới cho mỗi phiên sử dụng thông thường.

## Giới hạn hiện tại

- Host chỉ hỗ trợ Windows 10/11.
- Hiện capture màn hình chính, chưa có UI chọn màn hình/cửa sổ.
- Video truyền qua WebSocket/TCP nên vẫn có head-of-line blocking khi Wi-Fi mất gói;
  WebRTC là hướng nâng cấp tiếp theo.
- Đường capture vẫn có một lần copy GPU → system memory trước encoder.
- Hành vi touch cuối cùng còn phụ thuộc ứng dụng/game và cấu hình BlueStacks.

## Cấu trúc repository

```text
.
├─ host/
│  ├─ src/
│  │  ├─ app/
│  │  ├─ capture/
│  │  ├─ encoder/
│  │  ├─ input/
│  │  ├─ network/
│  │  ├─ protocol/
│  │  └─ streaming/
│  └─ tests/
├─ web/
│  ├─ js/
│  │  ├─ input/
│  │  ├─ network/
│  │  ├─ protocol/
│  │  └─ video/
│  └─ styles/
├─ CMakeLists.txt
└─ README.md
```
