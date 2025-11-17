/*
  Esp32 Pzem+ds18b20+relay - Fixed for PZEM-004T v3.0
  - AP+STA, web UI /live + /data
  - PZEM on Serial2 via HardwareSerial(2)
  - DS18B20 on OneWire, MLX90614 on I2C
  - Relay control + safety logic
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include <Adafruit_MLX90614.h>

// ----- PINS -----
#define PZEM_RX_PIN 5  // RX của ESP32 (input-only ok)
#define PZEM_TX_PIN 16
#define DS18B20_PIN 32
#define RELAY_PIN 33
#define SDA_PIN 18
#define SCL_PIN 19
#define RESET_BTN_PIN 26

// ----- WiFi/AP -----
const char* apSSID = "MTri_Sac";
const char* apPass = "11111111";
WebServer server(80);
Preferences prefs;

// ----- Sensors -----
OneWire oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);
Adafruit_MLX90614 mlx;

// ----- PZEM -----
// Use HardwareSerial instance for Serial2
HardwareSerial PZEMSerial(2);
PZEM004Tv30 pzem(&PZEMSerial, PZEM_RX_PIN, PZEM_TX_PIN);

// ----- Config/state -----
String savedSSID;
String savedPass;
String savedServerUrl;
bool useHttps = false;

unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL = 15000UL; // ms

bool charging = false;
unsigned long chargeStopTime = 0; // when stopped
const unsigned long RECHARGE_DELAY = 3600000UL; // 1 hour

// defaults
const float DEFAULT_MAX_TEMP_DS = 60.0f;
const float DEFAULT_MAX_TEMP_MLX = 55.0f;
uint16_t wait_minutes_default = 30;
uint16_t measure_seconds_default = 60;
float full_power_threshold_default = 5.0f;

// runtime configurable (loaded from prefs)
uint16_t wait_minutes;
uint16_t measure_seconds;
float full_power_threshold;

// helpers
void startAP(){
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID, apPass);
  Serial.print("AP started: "); Serial.println(WiFi.softAPIP());
}

String getConfigPage(){
  int wait_cfg = prefs.getInt("wait", (int)wait_minutes);
  int measure_cfg = prefs.getInt("measure", (int)measure_seconds);
  float full_cfg = prefs.getFloat("full", full_power_threshold);
  float tds_cfg = prefs.getFloat("t_ds", DEFAULT_MAX_TEMP_DS);
  float tmlx_cfg = prefs.getFloat("t_mlx", DEFAULT_MAX_TEMP_MLX);

  String checked = useHttps ? "checked" : "";

  String page = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body{background:#f2f2f2;font-family:system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial;margin:0;padding:20px}
    .card{background:#fff;max-width:520px;margin:24px auto;padding:20px;border-radius:12px;box-shadow:0 6px 20px rgba(0,0,0,0.08)}
    h2{margin:0 0 12px;color:#007bff;text-align:center}
    label{display:block;margin-top:8px;font-weight:600}
    input[type=text], input[type=password], input[type=number]{width:100%;padding:10px;margin-top:6px;border:1px solid #ddd;border-radius:8px}
    .row{display:flex;gap:10px}
    .row .col{flex:1}
    input[type=submit]{margin-top:12px;background:#28a745;color:#fff;border:0;padding:10px 16px;border-radius:8px;font-weight:700;cursor:pointer}
    footer{font-size:12px;color:#666;margin-top:12px;text-align:center}
  </style>
  <title>Cấu Hình</title>
</head>
<body>
  <div class="card">
    <h2>Cấu hình</h2>
    <form method="POST" action="/save">
      <label>SSID (WiFi nhà)</label>
      <input name="ssid" type="text" value=")rawliteral";
  page += savedSSID;
  page += R"rawliteral(" />
      <label>Password (để trống nếu giữ nguyên)</label>
      <input name="pass" type="password" placeholder="(leave blank to keep)">
      <label>Server URL (http(s)://host/path)</label>
      <input name="server" type="text" value=")rawliteral";
  page += savedServerUrl;
  page += R"rawliteral(" />
      <label>Use HTTPS</label>
      <input type="checkbox" name="usehttps" )rawliteral";
  page += checked;
  page += R"rawliteral(>

      <label>Thời gian chờ (phút) trước khi bật sạc</label>
      <input name="wait" type="number" min="0" value=")rawliteral";
  page += String(wait_cfg);
  page += R"rawliteral(" />

      <label>Thời gian đo công suất (giây)</label>
      <input name="measure" type="number" min="1" value=")rawliteral";
  page += String(measure_cfg);
  page += R"rawliteral(" />

      <label>Ngưỡng công suất đầy (W)</label>
      <input name="full" type="number" step="0.1" value=")rawliteral";
  page += String(full_cfg);
  page += R"rawliteral(" />

      <div class="row">
        <div class="col">
          <label>Nhiệt tối đa bộ sạc (°C)</label>
          <input name="t_ds" type="number" step="0.1" value=")rawliteral";
  page += String(tds_cfg);
  page += R"rawliteral(" />
        </div>
        <div class="col">
          <label>Nhiệt tối đa vỏ bình (°C)</label>
          <input name="t_mlx" type="number" step="0.1" value=")rawliteral";
  page += String(tmlx_cfg);
  page += R"rawliteral(" />
        </div>
      </div>

      <input type="submit" value="Lưu cấu hình">
    </form>
    <br>
    <a href="/live">Xem Thông Số</a>
    <footer>Nhấn giữ nút RESET (>2s) để xóa cấu hình. Nhấn nhanh trong lúc chờ để bỏ qua.</footer>
  </div>
</body>
</html>
)rawliteral";
  return page;
}

void handleRoot(){
  server.send(200, "text/html", getConfigPage());
}

void handleSave(){
  // read args
  savedSSID = server.arg("ssid");
  String passArg = server.arg("pass");
  savedServerUrl = server.arg("server");
  useHttps = server.hasArg("usehttps");

  prefs.putString("ssid", savedSSID);
  if(passArg.length() > 0){
    prefs.putString("pass", passArg);
    savedPass = passArg; // update runtime copy
  }
  prefs.putString("server", savedServerUrl);
  prefs.putBool("https", useHttps);

  int w = server.arg("wait").toInt(); if(w >= 0) prefs.putInt("wait", w);
  int m = server.arg("measure").toInt(); if(m > 0) prefs.putInt("measure", m);
  float f = server.arg("full").toFloat(); if(f > 0) prefs.putFloat("full", f);

  float tds = server.arg("t_ds").toFloat(); if(tds > 0) prefs.putFloat("t_ds", tds);
  float tmlx = server.arg("t_mlx").toFloat(); if(tmlx > 0) prefs.putFloat("t_mlx", tmlx);

  server.send(200, "text/html", "Saved. Rebooting...");
  delay(800);
  ESP.restart();
}

bool connectSavedWiFi(){
  if(savedSSID.length() == 0) return false;
  Serial.printf("Connecting to SSID: %s\n", savedSSID.c_str());
  WiFi.begin(savedSSID.c_str(), savedPass.c_str());
  unsigned long start = millis();
  while(millis() - start < 20000UL){
    if(WiFi.status() == WL_CONNECTED) return true;
    delay(500);
  }
  return false;
}

String getLivePage(){
  String page = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body{background:#f2f2f2;font-family:system-ui;margin:0;padding:20px}
    .card{background:#fff;max-width:420px;margin:24px auto;padding:20px;border-radius:12px;
          box-shadow:0 6px 20px rgba(0,0,0,0.08);text-align:center}
    .val{font-size:20px;font-weight:700;margin:6px 0}
    a.back{display:block;margin-top:20px;background:#28a745;color:#fff;padding:10px;
           border-radius:8px;text-decoration:none;font-weight:600}
  </style>
</head>
<body>
  <div class="card">
    <h2>Realtime Monitor</h2>
    <div>Nhiệt Bộ Sạc: <div id="ds" class="val">-- °C</div></div>
    <div>Nhiệt Vỏ Bình: <div id="mlx" class="val">-- °C</div></div>
    <div>Voltage: <div id="v" class="val">-- V</div></div>
    <div>Current: <div id="i" class="val">-- A</div></div>
    <div>Power: <div id="p" class="val">-- W</div></div>

    <a class="back" href="/">← Quay lại menu</a>
  </div>
<script>
async function update(){
  try{
    const r = await fetch('/data');
    const j = await r.json();
    document.getElementById('ds').innerText = j.ds + ' °C';
    document.getElementById('mlx').innerText = j.mlx + ' °C';
    document.getElementById('v').innerText = j.v + ' V';
    document.getElementById('i').innerText = j.i + ' A';
    document.getElementById('p').innerText = j.p + ' W';
  }catch(e){ console.log(e); }
}
setInterval(update,1000);
update();
</script>
</body>
</html>
)rawliteral";
  return page;
}

void handleLive(){
  server.send(200, "text/html", getLivePage());
}

void handleData(){
  float ds = NAN;
  float mx = NAN;
  float v = NAN;
  float i = NAN;
  float p = NAN;
  // read sensors quickly
  sensors.requestTemperatures();
  ds = sensors.getTempCByIndex(0);
  mx = mlx.readObjectTempC();
  v = pzem.voltage();
  i = pzem.current();
  p = pzem.power();

  String j = "{";
  j += "\"ds\":" + String(isnan(ds)?0:ds,2);
  j += ",\"mlx\":" + String(isnan(mx)?0:mx,2);
  j += ",\"v\":" + String(isnan(v)?0:v,2);
  j += ",\"i\":" + String(isnan(i)?0:i,3);
  j += ",\"p\":" + String(isnan(p)?0:p,2);
  j += "}";
  server.send(200, "application/json", j);
}

void clearConfig(){ prefs.clear(); }

void setup(){
  Serial.begin(115200);
  delay(100);

  prefs.begin("cfg", false);
  savedSSID = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  savedServerUrl = prefs.getString("server", "");
  useHttps = prefs.getBool("https", false);

  wait_minutes = (uint16_t)prefs.getInt("wait", (int)wait_minutes_default);
  measure_seconds = (uint16_t)prefs.getInt("measure", (int)measure_seconds_default);
  full_power_threshold = prefs.getFloat("full", full_power_threshold_default);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  digitalWrite(RELAY_PIN, LOW); // keep relay OFF initially

  sensors.begin();
  Wire.begin(SDA_PIN, SCL_PIN);
  mlx.begin();

  // start Serial2 for PZEM (use PZEMSerial instance)
  PZEMSerial.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);
  delay(200);

  // start AP+STA so AP is always available while trying to connect
  startAP();

  // register web handlers (so user can open config during wait)
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/live", handleLive);
  server.on("/data", handleData);
  server.begin();

  // try connect saved WiFi (AP remains available)
  if(connectSavedWiFi()){
    Serial.print("Connected to WiFi, IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("Not connected to WiFi - AP available for config");
  }

  // STARTUP WAIT: keep relay OFF for wait_minutes unless user presses RESET short to skip
  Serial.printf("Startup: waiting %u minutes before enabling relay (short press RESET to skip)\n", wait_minutes);
  unsigned long wait_ms = (unsigned long)wait_minutes * 60000UL;
  unsigned long start_wait = millis();
  while(millis() - start_wait < wait_ms){
    server.handleClient(); // allow config page during wait
    // detect short press
    if(digitalRead(RESET_BTN_PIN) == LOW){
      unsigned long t0 = millis();
      while(digitalRead(RESET_BTN_PIN) == LOW){
        if(millis() - t0 > 2500UL) break; // long press will be handled in loop
        delay(50);
      }
      if(millis() - t0 < 2000UL){
        Serial.println("Short RESET press detected - skipping wait");
        break;
      }
    }
    delay(200);
  }

  // After wait -> enable relay for measure_seconds to read power
  Serial.printf("Enable relay for %u seconds to measure power...\n", measure_seconds);
  digitalWrite(RELAY_PIN, HIGH);
  unsigned long measure_start = millis();
  unsigned long measure_ms = (unsigned long)measure_seconds * 1000UL;
  while(millis() - measure_start < measure_ms){
    float voltage = pzem.voltage();
    float current = pzem.current();
    float power = pzem.power();
    Serial.printf("Measure: V=%.2f V, I=%.3f A, P=%.2f W\n", voltage, current, power);
    server.handleClient();
    delay(2000);
  }

  // final decision
  float tempDS = NAN; float tempMLX = NAN; float power_now = NAN;
  sensors.requestTemperatures(); tempDS = sensors.getTempCByIndex(0);
  tempMLX = mlx.readObjectTempC();
  power_now = pzem.power();

  float tds_cfg = prefs.getFloat("t_ds", DEFAULT_MAX_TEMP_DS);
  float tmlx_cfg = prefs.getFloat("t_mlx", DEFAULT_MAX_TEMP_MLX);

  if((isnan(tempDS) || tempDS < tds_cfg)
     && (isnan(tempMLX) || tempMLX < tmlx_cfg)
     && (isnan(power_now) || power_now > full_power_threshold)){
    digitalWrite(RELAY_PIN, HIGH);
    charging = true;
    Serial.println("Continue charging: relay kept ON.");
  } else {
    digitalWrite(RELAY_PIN, LOW);
    charging = false;
    chargeStopTime = millis();
    Serial.println("Do not charge: relay turned OFF.");
  }
}

// --- sendToServer with optional HTTPS support ---
void sendToServer(float tds, float tmlx, float v, float i, float p){
  if(savedServerUrl.length() == 0) return;

  HTTPClient http;
  String json = "{";
  json += "\"ds\":" + String(isnan(tds)?0:tds,2) + ",";
  json += "\"mlx\":" + String(isnan(tmlx)?0:tmlx,2) + ",";
  json += "\"v\":" + String(isnan(v)?0:v,2) + ",";
  json += "\"i\":" + String(isnan(i)?0:i,3) + ",";
  json += "\"p\":" + String(isnan(p)?0:p,2);
  json += "}";

  if(useHttps){
    // insecure: accept any cert (convenient for testing)
    WiFiClientSecure *client = new WiFiClientSecure();
    client->setInsecure();
    if(http.begin(*client, savedServerUrl)){
      http.addHeader("Content-Type", "application/json");
      http.POST(json);
      http.end();
    }
    delete client;
  } else {
    if(http.begin(savedServerUrl)){
      http.addHeader("Content-Type", "application/json");
      http.POST(json);
      http.end();
    }
  }
}

void loop(){
  // handle webserver
  server.handleClient();

  // reset button handling: long press >2s clears prefs and reboots
  static unsigned long btnDownTime = 0;
  if(digitalRead(RESET_BTN_PIN) == LOW){
    if(btnDownTime == 0) btnDownTime = millis();
    else if(millis() - btnDownTime > 2000UL){
      Serial.println("Long RESET press detected - clearing config and rebooting");
      clearConfig();
      delay(200);
      ESP.restart();
    }
  } else {
    btnDownTime = 0;
  }

  // sensor reads and safety logic
  float tempDS = NAN; float tempMLX = NAN; float voltage = NAN; float current = NAN; float power = NAN;
  sensors.requestTemperatures(); tempDS = sensors.getTempCByIndex(0);
  tempMLX = mlx.readObjectTempC();
  voltage = pzem.voltage(); current = pzem.current(); power = pzem.power();

  float tds_cfg = prefs.getFloat("t_ds", DEFAULT_MAX_TEMP_DS);
  float tmlx_cfg = prefs.getFloat("t_mlx", DEFAULT_MAX_TEMP_MLX);

  // over temperature
  if((!isnan(tempDS) && tempDS > tds_cfg) || (!isnan(tempMLX) && tempMLX > tmlx_cfg)){
    if(charging){
      digitalWrite(RELAY_PIN, LOW);
      charging = false;
      chargeStopTime = millis();
      Serial.println("Stop charge: over temperature!");
    }
  }

  // full detection
  if(!isnan(power) && power < full_power_threshold && charging){
    digitalWrite(RELAY_PIN, LOW);
    charging = false;
    chargeStopTime = millis();
    Serial.println("Stop charge: battery full (power < threshold)");
  }

  // recharge delay logic
  if(!charging && (millis() - chargeStopTime > RECHARGE_DELAY)){
    // before re-enabling, do a short measurement cycle: enable 1 minute and assess
    Serial.println("Recharge delay passed - performing 1-minute measure before re-enable");
    digitalWrite(RELAY_PIN, HIGH);
    unsigned long ms0 = millis();
    while(millis() - ms0 < 60000UL){
      float v = pzem.voltage(); float i = pzem.current(); float p = pzem.power();
      Serial.printf("Recheck: V=%.2f I=%.3f P=%.2f\n", v, i, p);
      delay(2000);
    }
    float tempD = NAN; float tempM = NAN; float pnow = NAN;
    sensors.requestTemperatures(); tempD = sensors.getTempCByIndex(0);
    tempM = mlx.readObjectTempC(); pnow = pzem.power();
    if((isnan(tempD) || tempD < tds_cfg) && (isnan(tempM) || tempM < tmlx_cfg) && (isnan(pnow) || pnow > full_power_threshold)){
      digitalWrite(RELAY_PIN, HIGH); charging = true; Serial.println("Re-enable charging.");
    } else {
      digitalWrite(RELAY_PIN, LOW); charging = false; chargeStopTime = millis(); Serial.println("Re-enable blocked by safety or full");
    }
  }

  // telemetry
  unsigned long now = millis();
  if(now - lastSend >= SEND_INTERVAL){
    lastSend = now;
    if(WiFi.status() == WL_CONNECTED){
      sendToServer(tempDS, tempMLX, voltage, current, power);
    }
    Serial.printf("Telemetry: DS=%.2fC MLX=%.2fC V=%.2fV I=%.3fA P=%.2fW\n",
                  (isnan(tempDS)?0:tempDS),
                  (isnan(tempMLX)?0:tempMLX),
                  (isnan(voltage)?0:voltage),
                  (isnan(current)?0:current),
                  (isnan(power)?0:power));
  }

  delay(100);
}
