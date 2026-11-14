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
      if (s.ms - alarmAt < 8000) return true;
      phase = Tracking;
    }
    if (phase == Impact && s.ms - impactAt > 1500) phase = Tracking;
    if (phase == Tracking && s.magnitude >= 2.2f) {
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
enum Command { Auto, Manual, LedOn, LedOff, BuzzerOn, BuzzerOff, TestOutput };
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
  uint32_t lastPrint = 0, testStarted = 0, lastProbe = millis() - 1000;
  bool testing = false, sensorReady = false;
  for (;;) {
    const uint32_t now = millis();
    if (!sensorReady && now - lastProbe >= 1000) {
      lastProbe = now;
      sensorReady = writeRegister(0x6B, 0x01) && writeRegister(0x1C, 0x10);
      Serial.println(sensorReady ? "MPU6050 OK" : "MPU6050 I2C ERROR: check 3V3, GND, SDA21, SCL22");
    }
    while (Serial.available()) {
      const int key = Serial.read();
      if (key == 'o' || key == 'O') {
        const Command cmd = TestOutput;
        xQueueSend(commandQueue, &cmd, 0);
      }
      if ((key == 'f' || key == 'F') && !testing) {
        const Command cmd = Auto;
        xQueueSend(commandQueue, &cmd, 0);
        testStarted = now;
        testing = true;
        Serial.println("FALL TEST: standing -> impact -> horizontal");
      }
    }
    Sample s{};
    const uint32_t elapsed = now - testStarted;
    if (testing && elapsed >= 1800) testing = false;
    bool valid;
    if (testing) {
      if (elapsed < 700) s = {now, 0, 0, 1, 1, 0};
      else if (elapsed < 900) s = {now, 3, 0, 1, sqrtf(10), 71.6f};
      else s = {now, 1, 0, 0, 1, 90};
      valid = true;
    } else {
      valid = sensorReady && readSample(s);
    }
    // An invalid sample breaks posture confirmation and allows an active alarm to expire.
    if (!valid) s.ms = now;
    if (xQueueSend(dataAlert, &s, 0) != pdPASS) Serial.println("data Alert: queue full");
    if (valid) {
      xQueueOverwrite(dataQueue, &s);
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
  bool outputTest = false;
  uint32_t outputTestAt = 0;
  for (;;) {
    Sample s{};
    while (xQueueReceive(dataAlert, &s, 0) == pdPASS) {
      const bool fall = detector.update(s);
      if (fall && !state.fall) Serial.println("FALL DETECTED");
      state.fall = fall;
    }

    Command cmd;
    while (xQueueReceive(commandQueue, &cmd, 0) == pdPASS) {
      switch (cmd) {
        case Auto: state.manual = manualLed = manualBuzzer = false; break;
        case Manual: state.manual = true; break;
        case LedOn: if (state.manual) manualLed = true; break;
        case LedOff: if (state.manual) manualLed = false; break;
        case BuzzerOn: if (state.manual) manualBuzzer = true; break;
        case BuzzerOff: if (state.manual) manualBuzzer = false; break;
        case TestOutput:
          outputTest = true;
          outputTestAt = millis();
          Serial.println("OUTPUT TEST: LED + buzzer ON for 3 s");
          break;
      }
    }

    if (outputTest && millis() - outputTestAt >= 3000) outputTest = false;
    const bool led = outputTest || (state.manual ? manualLed : state.fall);
    const bool buzzer = outputTest || (state.manual ? manualBuzzer : state.fall);
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
        Serial.printf("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
        if (!mqtt.connect(clientId)) {
          Serial.printf("MQTT connect failed, state=%d\n", mqtt.state());
        } else if (!mqtt.subscribe(commandTopic, 1)) {
          Serial.println("MQTT subscribe failed");
          mqtt.disconnect();
        } else {
          Serial.println("MQTT connected");
        }
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
  Serial.println("Enter o to test outputs, f to simulate a fall");
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  Wire.begin(21, 22);
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
