# Lab 3 – Đề 4: Mạch tự động giám sát thông số môi trường cho bể nuôi thủy, hải sản

**ESP32 + FreeRTOS + MQTT + Web Dashboard** (mô phỏng trên Wokwi)

- Sinh viên thực hiện: **Nguyễn Ngọc Thái**
- MSSV: **231432343**
- Lớp: **Kĩ Sư Kĩ Thuật Viễn Thông K64**

---

## 1. Giới thiệu

Đề tài thiết kế hệ thống giám sát bể nuôi thủy, hải sản. ESP32 đo **mực nước** bằng cảm biến siêu âm HC-SR04 và đo **nhiệt độ bể** bằng DS18B20, hiển thị qua Serial Monitor, đồng thời gửi dữ liệu lên **MQTT Broker** qua WiFi. Một trang web hiển thị dữ liệu thời gian thực và cho phép điều khiển **máy bơm nước vào** và **van thoát nước ra**.

Hệ thống chạy trên **FreeRTOS** với 3 task chia cho 2 core của ESP32, giao tiếp với nhau qua hàng đợi (queue).

## 2. Chức năng

- Đọc mực nước (HC-SR04) và nhiệt độ bể (DS18B20) mỗi 2 giây, in ra Serial.
- Gửi dữ liệu lên MQTT Broker (HiveMQ) dạng JSON.
- Chế độ **Tự động**: bật/tắt bơm và van theo vùng mực nước.
- Chế độ **Thủ công**: điều khiển bơm và van bằng nút bấm trên web.
- Web dashboard: 2 thẻ số (mực nước, nhiệt độ), hình bể nước trực quan, 2 biểu đồ thời gian thực, nút chuyển chế độ, nút bơm và nút van.
- Có chế độ "Dữ liệu giả lập" trên web để thử giao diện khi chưa chạy mô phỏng.

## 3. Phần cứng và sơ đồ chân

Bo mạch: ESP32 DevKit C (mô phỏng trên Wokwi). Hai LED giả lập thiết bị chấp hành: LED xanh là máy bơm, LED đỏ là van thoát.

| Thiết bị | Chân thiết bị | Chân ESP32 |
|---|---|---|
| HC-SR04 | VCC / GND | 5V / GND |
| HC-SR04 | TRIG / ECHO | GPIO5 / GPIO18 |
| DS18B20 | VCC / GND | 3V3 / GND |
| DS18B20 | DQ | GPIO4 |
| LED xanh (bơm) | Anode qua R 220 Ω | GPIO25 (PWM 5 kHz, 8 bit) |
| LED đỏ (van) | Anode qua R 220 Ω | GPIO26 |
| Hai LED | Cathode | GND |

> Trên mạch thật, DS18B20 cần điện trở kéo lên 4,7 kΩ giữa DQ và 3V3 (Wokwi không bắt buộc).

Mực nước được tính từ khoảng cách đo: **mực nước = 2,5 m − khoảng cách** (cảm biến đặt cao 2,5 m so với đáy bể).

## 4. Kiến trúc FreeRTOS

| Task | Nhiệm vụ | Core | Ưu tiên | Stack | Chu kỳ |
|---|---|---|---|---|---|
| `Truyen` (Task truyền data) | Kết nối WiFi/MQTT, nhận lệnh từ web, đọc `dataQueue` và publish JSON | 0 | 1 | 8192 | Chờ queue tối đa 50 ms |
| `Doc` (Task đọc data) | Đọc HC-SR04 và DS18B20, in Serial, gửi hai queue | 1 | 2 | 4096 | 2 s |
| `CanhBao` (Task cảnh báo) | Đọc `alertQueue`, chia vùng mực nước, điều khiển bơm/van | 1 | 2 | 4096 | Chờ queue tối đa 100 ms |

| Queue | Kiểu phần tử | Độ sâu | Gửi từ | Nhận ở |
|---|---|---|---|---|
| `dataQueue` | `struct SensorData {level, temp}` | 5 | Task đọc | Task truyền |
| `alertQueue` | `float` (mực nước) | 5 | Task đọc | Task cảnh báo |

Lệnh điều khiển thủ công từ web được chuyển cho Task cảnh báo qua các biến dùng chung `autoMode`, `manualPump`, `manualValve`.

## 5. Thuật toán điều khiển (chế độ tự động)

| Vùng | Mực nước | Hành động | Thời gian |
|---|---|---|---|
| 0 | < 0,5 m | Mở van thoát | 10 s |
| 1 | 0,5 – 1 m | Không tác động | – |
| 2 | 1 – 1,5 m | Bơm 50% công suất | 10 s |
| 3 | 1,5 – 2 m | Bơm 100% công suất | 20 s |
| 4 | ≥ 2 m | Không tác động | – |

Chu kỳ bơm/xả chỉ bắt đầu khi mực nước **chuyển sang vùng mới**; hết thời gian thì tự tắt. Cách này tránh bật tắt liên tục khi mực nước dao động quanh một vùng.

## 6. Giao tiếp MQTT

Broker: `broker.hivemq.com` (ESP32 dùng cổng 1883, web dùng WebSocket bảo mật cổng 8884).

| Topic | Hướng | Nội dung |
|---|---|---|
| `lab3/de4/sensor` | ESP32 → web | JSON `{level, temp, pump, valve, auto}` |
| `lab3/de4/pump/set` | web → ESP32 | `ON` / `OFF` (chỉ khi thủ công) |
| `lab3/de4/valve/set` | web → ESP32 | `ON` / `OFF` (chỉ khi thủ công) |
| `lab3/de4/mode/set` | web → ESP32 | `AUTO` / `MANUAL` |

## 7. Cấu trúc thư mục

| Tệp | Mô tả |
|---|---|
| `sketch.ino` | Mã nguồn ESP32 (FreeRTOS, đọc cảm biến, điều khiển, MQTT) |
| `diagram.json` | Sơ đồ mạch Wokwi |
| `libraries.txt` | Danh sách thư viện dùng trong Wokwi |
| `wokwi-project.txt` | Thông tin dự án Wokwi |
| `index.html` | Giao diện web dashboard (một trang HTML duy nhất, dùng mqtt.js) |
| `Nguyễn Ngọc Thái lab3 Đề 4.docx` | Báo cáo Lab 3 – Đề 4 |

## 8. Hướng dẫn chạy

1. Mở dự án Wokwi (link ở mục 10), bấm **Start the simulation**. ESP32 tự kết nối mạng `Wokwi-GUEST` và MQTT broker.
2. Mở web dashboard (link ở mục 10) hoặc mở `index.html` bằng trình duyệt (cần có Internet). Khi dòng trạng thái báo "Đã kết nối broker", web nhận dữ liệu từ ESP32.
3. Trong Wokwi, bấm vào HC-SR04 và đổi thuộc tính **Distance** để thay đổi mực nước, rồi quan sát LED, Serial Monitor và web.
4. Trên web, chọn **Thủ công** để bật/tắt bơm và van bằng nút bấm.

> Nếu chưa chạy Wokwi, tick vào ô **Dữ liệu giả lập** trên web để xem thử giao diện.

## 9. Kết quả kiểm thử

Đổi khoảng cách HC-SR04 trong Wokwi (mực nước = 2,5 m − khoảng cách):

| Khoảng cách đặt | Mực nước | Vùng | Kết quả |
|---|---|---|---|
| 2,25 m | 0,23 m | 0 | Bơm tắt, van mở (LED đỏ sáng) |
| 1,83 m | 0,65 m | 1 | Bơm và van đều tắt |
| 1,3 m | 1,19 m | 2 | Bơm 50% (LED xanh sáng) |
| 0,81 m | 1,68 m | 3 | Bơm 100%, van đóng |
| 0,31 m | 2,19 m | 4 | Bơm và van đều tắt |
| Chế độ thủ công | – | – | Nút web bật/tắt được bơm và van |

Cả 6 kịch bản cho kết quả đúng như mong đợi. Hình ảnh chi tiết có trong file báo cáo `.docx`.

## 10. Liên kết

- Dự án Wokwi: https://wokwi.com/projects/480589827552788481
- Web dashboard: https://ngocthai2k5.github.io/NguyenNgocThai_Lab3_De4/
