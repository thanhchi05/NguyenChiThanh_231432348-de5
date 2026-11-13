#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <math.h>

struct Sample {
  uint32_t ms;
  float x, y, z, magnitude, tilt;
};

class FallDetector {
  enum Phase { Tracking, Impact, Holding };
  Phase phase = Tracking;
  uint32_t impactAt = 0, tiltedAt = 0, alarmAt = 0;
  float referenceTilt = 0;
  bool confirming = false;

public:
  bool update(const Sample& s) {
    if (phase == Holding) {
      Serial.println("FALL DETECTED");
      if (s.ms - alarmAt < 8000) return true;
      phase = Tracking;
    }
    if (phase == Impact && s.ms - impactAt > 1500) phase = Tracking;
    if (phase == Tracking && s.magnitude >= 1.8f) {
      impactAt = s.ms;
      confirming = false;
      phase = Impact;
    }
    if (phase == Impact) {
      const bool tilted = s.tilt >= 60 && fabsf(s.tilt - referenceTilt) >= 45;
      const bool stableMagnitude = s.magnitude >= 0.8f && s.magnitude <= 1.2f;
      if (tilted && stableMagnitude) {
        if (!confirming) tiltedAt = s.ms;
        confirming = true;
        if (s.ms - tiltedAt >= 350) {
          alarmAt = s.ms;
          phase = Holding;
          return true;
        }
      } else {
        confirming = false;
      }
    }
    if (phase == Tracking && s.magnitude >= 0.8f && s.magnitude <= 1.2f) {
      referenceTilt = s.tilt;
    }
    return false;
  }
};

constexpr char WIFI_SSID[] = "Wokwi-GUEST";
constexpr char WIFI_PASSWORD[] = "";
constexpr char BROKER[] = "broker.emqx.io";
constexpr char TOPIC[] = "utc/fall/8c317a82dd624959";
constexpr uint8_t MPU = 0x68;
constexpr int LED_PIN = 25;
constexpr int BUZZER_PIN = 26;

struct OutputState { bool manual, led, buzzer, fall; };
enum Command { Auto, Manual, LedOn, LedOff, BuzzerOn, BuzzerOff };
QueueHandle_t dataQueue, dataAlert, commandQueue, stateQueue;
WiFiClient tcp;
PubSubClient mqtt(tcp);
char commandTopic[96], telemetryTopic[96];

bool writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

float readAxis() {
  const uint16_t hi = Wire.read();
  const uint16_t lo = Wire.read();
  return static_cast<int16_t>((hi << 8) | lo) / 4096.0f; // ±8 g
}

bool readSample(Sample& s) {
  Wire.beginTransmission(MPU);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(MPU, uint8_t(6)) != 6) return false;
  s.ms = millis();
  s.x = readAxis();
  s.y = readAxis();
  s.z = readAxis();
  s.magnitude = sqrtf(s.x*s.x + s.y*s.y + s.z*s.z);
  s.tilt = atan2f(hypotf(s.x, s.y), s.z) * RAD_TO_DEG;
  return true;
}

void readTask(void*) {
  TickType_t wake = xTaskGetTickCount();
  uint32_t lastPrint = 0;
  for (;;) {
    Sample s{};
    if (readSample(s)) {
      xQueueOverwrite(dataQueue, &s);
      if (xQueueSend(dataAlert, &s, 0) != pdPASS) Serial.println("data Alert: queue full");
      if (s.ms - lastPrint >= 250) {
        lastPrint = s.ms;
        Serial.printf("ax=%.2f ay=%.2f az=%.2f |a|=%.2f g tilt=%.1f deg\n",
                      s.x, s.y, s.z, s.magnitude, s.tilt);
      }
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
  }
}

void alertTask(void*) {
  FallDetector detector;
  OutputState state{};
  bool manualLed = false, manualBuzzer = false;
  for (;;) {
    Sample s{};
    while (xQueueReceive(dataAlert, &s, 0) == pdPASS) state.fall = detector.update(s);

    Command cmd;
    while (xQueueReceive(commandQueue, &cmd, 0) == pdPASS) {
      switch (cmd) {
        case Auto: state.manual = manualLed = manualBuzzer = false; break;
        case Manual: state.manual = true; break;
        case LedOn: if (state.manual) manualLed = true; break;
        case LedOff: if (state.manual) manualLed = false; break;
        case BuzzerOn: if (state.manual) manualBuzzer = true; break;
        case BuzzerOff: if (state.manual) manualBuzzer = false; break;
      }
    }

    const bool led = state.manual ? manualLed : state.fall;
    const bool buzzer = state.manual ? manualBuzzer : state.fall;
    if (led != state.led) digitalWrite(LED_PIN, led);
    if (buzzer != state.buzzer) {
      if (buzzer) tone(BUZZER_PIN, 2200);
      else noTone(BUZZER_PIN);
    }
    state.led = led;
    state.buzzer = buzzer;
    xQueueOverwrite(stateQueue, &state);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void onCommand(char* topic, uint8_t* payload, unsigned int size) {
  if (strcmp(topic, commandTopic)) return;
  const char* names[] = {"auto", "manual", "led:on", "led:off", "buzzer:on", "buzzer:off"};
  for (unsigned int i = 0; i < sizeof(names)/sizeof(names[0]); ++i) {
    if (size == strlen(names[i]) && !memcmp(payload, names[i], size)) {
      const Command command = static_cast<Command>(i);
      if (xQueueSend(commandQueue, &command, 0) != pdPASS) Serial.println("Command queue full");
      return;
    }
  }
}

void transmitTask(void*) {
  snprintf(commandTopic, sizeof(commandTopic), "%s/cmd", TOPIC);
  snprintf(telemetryTopic, sizeof(telemetryTopic), "%s/telemetry", TOPIC);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  mqtt.setServer(BROKER, 1883);
  mqtt.setCallback(onCommand);
  mqtt.setSocketTimeout(2);
  char clientId[48];
  snprintf(clientId, sizeof(clientId), "fall-esp-%08lx", (unsigned long)esp_random());
  uint32_t nextAttempt = millis(), lastSend = 0;
  for (;;) {
    const uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED) {
      if (!mqtt.connected() && int32_t(now - nextAttempt) >= 0) {
        nextAttempt = now + 3000;
        if (mqtt.connect(clientId) && !mqtt.subscribe(commandTopic, 1)) mqtt.disconnect();
      }
      if (mqtt.connected() && mqtt.loop() && now - lastSend >= 250) {
        lastSend = now;
        Sample s;
        OutputState out;
        if (xQueueReceive(dataQueue, &s, 0) == pdPASS &&
            xQueuePeek(stateQueue, &out, 0) == pdPASS) {
          char payload[224];
          snprintf(payload, sizeof(payload),
                   "{\"ms\":%lu,\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,\"g\":%.3f,\"tilt\":%.2f,"
                   "\"fall\":%s,\"manual\":%s,\"led\":%s,\"buzzer\":%s}",
                   (unsigned long)s.ms, s.x, s.y, s.z, s.magnitude, s.tilt,
                   out.fall ? "true" : "false", out.manual ? "true" : "false",
                   out.led ? "true" : "false", out.buzzer ? "true" : "false");
          mqtt.publish(telemetryTopic, payload, false);
        }
      }
    } else if (mqtt.connected()) {
      tcp.stop();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  Wire.begin(21, 22);
  while (!writeRegister(0x6B, 0x01) || !writeRegister(0x1C, 0x10)) {
    Serial.println("MPU6050: check wiring and I2C address 0x68");
    delay(1000);
  }
  dataQueue = xQueueCreate(1, sizeof(Sample));
  dataAlert = xQueueCreate(24, sizeof(Sample));
  commandQueue = xQueueCreate(12, sizeof(Command));
  stateQueue = xQueueCreate(1, sizeof(OutputState));
  if (!dataQueue || !dataAlert || !commandQueue || !stateQueue) {
    Serial.println("Queue allocation failed");
    return;
  }
  if (xTaskCreatePinnedToCore(alertTask, "Task canh bao", 4096, nullptr, 3, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(readTask, "Task doc data", 4096, nullptr, 2, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(transmitTask, "Task truyen data", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
    Serial.println("Task creation failed");
  }
}

void loop() { delay(1000); }
