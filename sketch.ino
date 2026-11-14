#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ---------- Chân ----------
#define TRIG_PIN   5
#define ECHO_PIN   18
#define DS_PIN     4
#define PUMP_PIN   25   // PWM
#define VALVE_PIN  26

// ---------- Cấu hình ----------
const float TANK_HEIGHT = 2.5;          // m: từ cảm biến xuống đáy bể
const char* WIFI_SSID   = "Wokwi-GUEST";
const char* WIFI_PASS   = "";
const char* MQTT_SERVER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const char* TOPIC_SENSOR = "lab3/de4/sensor";
const char* TOPIC_PUMP   = "lab3/de4/pump/set";
const char* TOPIC_VALVE  = "lab3/de4/valve/set";
const char* TOPIC_MODE   = "lab3/de4/mode/set";   // AUTO | MANUAL

// ---------- Đối tượng ----------
OneWire oneWire(DS_PIN);
DallasTemperature sensors(&oneWire);
WiFiClient espClient;
PubSubClient mqtt(espClient);

struct SensorData { float level; float temp; };

QueueHandle_t dataQueue;    // đầy đủ dữ liệu -> Task truyền
QueueHandle_t alertQueue;   // mực nước       -> Task cảnh báo

volatile int pumpPercent = 0;     // trạng thái thực tế, để gửi lên web
volatile int valveState  = 0;
volatile bool autoMode   = true;  // true: tự động, false: thủ công (nút web)
volatile int manualPump  = 0;     // chỉ dùng khi thủ công: 0 = OFF, 1 = ON
volatile int manualValve = 0;

// ---------- Đo khoảng cách (m) ----------
float readDistanceM() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long us = pulseIn(ECHO_PIN, HIGH, 30000);
  if (us == 0) return -1;
  return us * 0.0343 / 2.0 / 100.0;   // us -> cm -> m
}

// ---------- Chia vùng mực nước ----------
int getBand(float lv) {
  if (lv < 0.5)  return 0;   // thấp   -> mở van 10s
  if (lv < 1.0)  return 1;   // không tác động
  if (lv < 1.5)  return 2;   // bơm 50% 10s
  if (lv < 2.0)  return 3;   // bơm 100% 20s
  return 4;                  // >= 2m: không tác động
}

void applyOutputs(int pumpPct, int valve) {
  ledcWrite(PUMP_PIN, pumpPct * 255 / 100);
  digitalWrite(VALVE_PIN, valve);
  pumpPercent = pumpPct;
  valveState  = valve;
}

// ---------- MQTT callback (chế độ + nút manual từ web) ----------
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim(); msg.toUpperCase();

  if (strcmp(topic, TOPIC_MODE) == 0) {
    bool toAuto = (msg == "AUTO");
    if (!toAuto && autoMode) {            // auto -> manual: giữ nguyên trạng thái đang chạy
      manualPump  = (pumpPercent > 0) ? 1 : 0;
      manualValve = valveState;
    }
    autoMode = toAuto;
    Serial.printf("[MODE] %s\n", toAuto ? "TU DONG" : "THU CONG");
    return;
  }
  if (autoMode) return;                   // chế độ tự động: bỏ qua nút bấm
  int v = (msg == "ON") ? 1 : 0;
  if (strcmp(topic, TOPIC_PUMP) == 0)  manualPump  = v;
  if (strcmp(topic, TOPIC_VALVE) == 0) manualValve = v;
}

// ================= TASK TRUYỀN DATA (Core 0) =================
void taskTruyen(void *pv) {
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!mqtt.connected()) {
      String id = "esp32-de4-" + String(random(0xffff), HEX);
      if (mqtt.connect(id.c_str())) {
        mqtt.subscribe(TOPIC_PUMP);
        mqtt.subscribe(TOPIC_VALVE);
        mqtt.subscribe(TOPIC_MODE);
        Serial.println("[MQTT] Connected");
      } else {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
    }
    mqtt.loop();
    SensorData d;
    if (xQueueReceive(dataQueue, &d, pdMS_TO_TICKS(50)) == pdTRUE) {
      char buf[160];
      snprintf(buf, sizeof(buf),
        "{\"level\":%.2f,\"temp\":%.1f,\"pump\":%d,\"valve\":%d,\"auto\":%d}",
        d.level, isnan(d.temp) ? -127.0 : d.temp, pumpPercent, valveState, autoMode ? 1 : 0);
      mqtt.publish(TOPIC_SENSOR, buf);
    }
  }
}

// ================= TASK ĐỌC DATA (Core 1) =================
void taskDoc(void *pv) {
  for (;;) {
    float dist = readDistanceM();
    sensors.requestTemperatures();
    float t = sensors.getTempCByIndex(0);
    if (t == DEVICE_DISCONNECTED_C) t = NAN;
    if (dist >= 0) {
      float level = TANK_HEIGHT - dist;
      if (level < 0) level = 0;
      SensorData d = { level, t };
      Serial.printf("Muc nuoc: %.2f m | Nhiet do: %.1f C | Che do: %s | Bom: %d%% | Van: %s\n",
                    level, t, autoMode ? "AUTO" : "MANUAL", pumpPercent, valveState ? "MO" : "DONG");
      xQueueSend(dataQueue, &d, 0);
      xQueueSend(alertQueue, &level, 0);
    } else {
      Serial.println("Loi doc HC-SR04");
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ================= TASK CẢNH BÁO (Core 1) =================
void taskCanhBao(void *pv) {
  float level;
  int lastBand = -1;
  bool wasAuto = true;
  int autoPump = 0, autoValve = 0;
  unsigned long pumpEnd = 0, valveEnd = 0;

  for (;;) {
    bool gotLevel = (xQueueReceive(alertQueue, &level, pdMS_TO_TICKS(100)) == pdTRUE);
    unsigned long now = millis();

    if (autoMode) {
      if (!wasAuto) {                      // vừa chuyển thủ công -> tự động
        wasAuto = true; lastBand = -1;     // đánh giá lại vùng ở lần đọc tiếp theo
      }
      if (gotLevel) {
        int band = getBand(level);
        if (band != lastBand) {            // chuyển vùng -> bắt đầu chu kỳ mới
          lastBand = band;
          autoPump = 0; autoValve = 0; pumpEnd = valveEnd = 0;
          switch (band) {
            case 0: autoValve = 1;   valveEnd = now + 10000; break;
            case 2: autoPump  = 50;  pumpEnd  = now + 10000; break;
            case 3: autoPump  = 100; pumpEnd  = now + 20000; break;
            default: break;
          }
          Serial.printf("[ALERT] Vung %d -> bom %d%%, van %d\n", band, autoPump, autoValve);
        }
      }
      if (pumpEnd  && now >= pumpEnd)  { pumpEnd = 0;  autoPump = 0;  }
      if (valveEnd && now >= valveEnd) { valveEnd = 0; autoValve = 0; }
      applyOutputs(autoPump, autoValve);
    } else {                               // thủ công: theo nút trên web
      wasAuto = false;
      autoPump = autoValve = 0; pumpEnd = valveEnd = 0;
      applyOutputs(manualPump * 100, manualValve);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(VALVE_PIN, OUTPUT);
  ledcAttach(PUMP_PIN, 5000, 8);   // core 3.x; core 2.x: ledcSetup(0,5000,8); ledcAttachPin(PUMP_PIN,0);
  sensors.begin();

  dataQueue  = xQueueCreate(5, sizeof(SensorData));
  alertQueue = xQueueCreate(5, sizeof(float));

  xTaskCreatePinnedToCore(taskTruyen,   "Truyen",   8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(taskDoc,      "Doc",      4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskCanhBao,  "CanhBao",  4096, NULL, 2, NULL, 1);
}

void loop() { vTaskDelay(portMAX_DELAY); }