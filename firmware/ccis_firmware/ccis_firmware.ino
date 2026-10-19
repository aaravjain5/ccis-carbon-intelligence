/*
  CCIS - ESP32 sensor node (Person 2)
  Reads: DHT22, MQ-135, ACS712-20A (via divider), HC-SR501 PIR
  Sends: JSON over HTTPS to Firebase Realtime Database (REST)
  Libraries (Library Manager): "DHT sensor library" (Adafruit) + "Adafruit Unified Sensor"
  Optional: Adafruit SSD1306 + Adafruit GFX (set USE_OLED 1)
  Board: "ESP32 Dev Module". Use core 2.x or 3.x.
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DHT.h>

#define USE_OLED  0
#define USE_RELAY 0

#if USE_OLED
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
  Adafruit_SSD1306 oled(128, 64, &Wire, -1);
#endif

// ---------- CONFIG ----------
#include "secrets.h"   // defines WIFI_SSID, WIFI_PASS, DB_URL, DEVICE_ID (git-ignored)

// ---------- PINS ----------
#define PIN_DHT   4     // DHT22 data
#define PIN_PIR   27    // PIR output (3.3V logic)
#define PIN_RELAY 26    // relay IN
#define PIN_GAS   34    // MQ-135 AO via divider (ADC1)
#define PIN_ACS   35    // ACS712 OUT via divider (ADC1)

// ---------- CALIBRATION ----------
const float DIV_FACTOR   = 1.5;     // 1/(20k/(10k+20k)) = 1.5
const float ACS_MV_PER_A = 100.0;   // 20A variant = 100 mV/A (NOT 185)
const float MAINS_V      = 230.0;   // assumed RMS mains voltage
const float DC_TEST_V    = 12.0;    // supply voltage of the DC test load (LOAD_IS_AC = false)
const float POWER_FACTOR = 1.0;     // 1.0 for resistive loads (bulb/heater)
const float CURRENT_DEADBAND = 0.15;// A, below this = treated as zero
const bool  LOAD_IS_AC   = true;    // false if testing with a DC load
const unsigned long SEND_MS = 15000;
const unsigned long WIFI_RETRY_MS = 10000; // min gap between non-blocking reconnect attempts
const unsigned long IDLE_AFTER_MS = 5UL * 60UL * 1000UL; // no motion for 5 min

DHT dht(PIN_DHT, DHT22);
float acsOffsetMv = 2500.0;
unsigned long lastSend = 0, lastMotion = 0;
double kwhTotal = 0, kwhIdle = 0;
bool relayOn = false;

float readAcsMvAvg(int n = 500) {
  double s = 0;
  for (int i = 0; i < n; i++) { s += analogReadMilliVolts(PIN_ACS) * DIV_FACTOR; delayMicroseconds(200); }
  return s / n;
}

float readCurrentA() {
  if (LOAD_IS_AC) {                       // RMS over 10 mains cycles (200 ms @ 50 Hz)
    double sum = 0, sumSq = 0; int n = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < 200) {
      float mv = analogReadMilliVolts(PIN_ACS) * DIV_FACTOR;
      sum += mv; sumSq += (double)mv * mv; n++;
    }
    // AC component only (std-dev about this window's own mean), so a drifting zero point adds no phantom current
    double mean = sum / n;
    float i = sqrt(max(sumSq / n - mean * mean, 0.0)) / ACS_MV_PER_A;
    return (i < CURRENT_DEADBAND) ? 0 : i;
  } else {                                // DC: average then subtract offset
    float i = fabs(readAcsMvAvg(300) - acsOffsetMv) / ACS_MV_PER_A;
    return (i < CURRENT_DEADBAND) ? 0 : i;
  }
}

int readGasRaw() {                        // average of 20 samples, 0-4095
  long s = 0;
  for (int i = 0; i < 20; i++) { s += analogRead(PIN_GAS); delay(5); }
  return s / 20;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(500); Serial.print("."); }
  Serial.println(WiFi.status() == WL_CONNECTED ? " connected" : " FAILED");
}

bool postJson(const String& body) {
  if (WiFi.status() != WL_CONNECTED) {    // never block loop(): the PIR must keep being polled during an outage
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry >= WIFI_RETRY_MS) { lastRetry = millis(); WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); }
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();                   // demo only; pin a certificate for production
  HTTPClient http;
  String url = String(DB_URL) + "/readings/" + DEVICE_ID + ".json";  // POST = push() with unique key
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  Serial.printf("POST %d\n", code);
  http.end();
  return code == 200;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PIR, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_GAS, ADC_11db);
  analogSetPinAttenuation(PIN_ACS, ADC_11db);
  dht.begin();
#if USE_RELAY
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, HIGH);          // most modules are active-LOW; HIGH = off
#endif
#if USE_OLED
  Wire.begin(21, 22);
  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
#endif
  Serial.println("Calibrating ACS712 zero - keep load OFF / disconnected...");
  delay(1000);
  acsOffsetMv = readAcsMvAvg(1000);
  Serial.printf("ACS offset = %.1f mV (expect ~2500)\n", acsOffsetMv);
#if USE_RELAY
  relayOn = true;                         // load energised now that calibration is done
  digitalWrite(PIN_RELAY, LOW);
#endif
  Serial.println("PIR warm-up 30 s...");
  delay(30000);
  connectWiFi();
  lastMotion = millis();
  lastSend = millis();
}

void loop() {
  if (digitalRead(PIN_PIR)) lastMotion = millis();

  if (millis() - lastSend >= SEND_MS) {
    float dtSec = (millis() - lastSend) / 1000.0;
    lastSend = millis();

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (isnan(t) || isnan(h)) { t = -99; h = -99; Serial.println("DHT read failed"); }

    int gas = readGasRaw();
    float amps = readCurrentA();
    float watts = LOAD_IS_AC ? amps * MAINS_V * POWER_FACTOR : amps * DC_TEST_V;
    bool occupied = (millis() - lastMotion) < IDLE_AFTER_MS;
    bool idleWaste = (watts > 1.0) && !occupied;

    double dk = watts * dtSec / 3.6e6;    // W*s -> kWh
    kwhTotal += dk;
    if (idleWaste) kwhIdle += dk;

#if USE_RELAY
    // Optional auto-cutoff: appliance running while room empty. Latched: once cut, stay off until
    // motion returns (otherwise 0 W -> no idle waste -> relay turns back on -> chatter every cycle).
    if (occupied) relayOn = true;
    else if (idleWaste) relayOn = false;
    digitalWrite(PIN_RELAY, relayOn ? LOW : HIGH);
#endif

    char body[320];
    snprintf(body, sizeof(body),
      "{\"temp\":%.1f,\"hum\":%.1f,\"gas_raw\":%d,\"current_a\":%.3f,\"power_w\":%.1f,"
      "\"occupied\":%d,\"idle_waste\":%d,\"kwh_total\":%.5f,\"kwh_idle\":%.5f,"
      "\"relay_on\":%d,\"ts\":{\".sv\":\"timestamp\"}}",
      t, h, gas, amps, watts, occupied, idleWaste, kwhTotal, kwhIdle, relayOn);
    Serial.println(body);
    postJson(body);

#if USE_OLED
    oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.printf("P:%.0fW I:%.2fA\nT:%.1f H:%.0f%%\nGas:%d  Occ:%d\nkWh:%.3f\nIdle:%.3f", watts, amps, t, h, gas, occupied, kwhTotal, kwhIdle);
    oled.display();
#endif
  }
}
