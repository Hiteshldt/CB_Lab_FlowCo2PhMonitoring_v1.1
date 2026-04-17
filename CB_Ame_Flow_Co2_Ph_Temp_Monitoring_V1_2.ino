#include <WiFi.h>
#include <WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// =====================================================================
// Carbelim AirForest — ESP32 Multi-Sensor Telemetry
// Firmware V1.2  (adds DS18B20 water temp + second MH-Z19E on Serial1)
// =====================================================================

// --- PIN DEFINITIONS ---
const int flowSensorPin = 27;
const int phSensorPin   = 32;     // ADC1_CH4 — safe with WiFi active
const int waterTempPin  = 4;      // DS18B20 1-Wire (needs 4.7k pull-up to 3V3)

// MH-Z19E #1  (INLET CO2):   Serial2, GPIO 16 (RX2) / GPIO 17 (TX2)
// MH-Z19E #2  (OUTLET CO2):  Serial1, remapped to GPIO 25 (RX1) / GPIO 26 (TX1)
const int co2OutletRxPin = 25;
const int co2OutletTxPin = 26;

// --- WI-FI ACCESS POINT ---
const char* ssid     = "Sensor_Dashboard";
const char* password = "password123";
WebServer server(80);

// --- GLOBAL SENSOR DATA ---
volatile int pulseCount    = 0;
float currentFlowRate      = 0.0;
int   currentCO2_inlet     = 0;
int   currentCO2_outlet    = 0;
int   currentCO2_absorbed  = 0;
float currentPH            = 0.0;
float currentPHVoltage     = 0.0;
float currentWaterTemp     = 0.0;
unsigned long oldTime      = 0;

// --- pH CALIBRATION (3.3V POWER) ---
// Calibrate with pH 7.00 and pH 4.00 buffers — see guide section 5.
#define PH_OFFSET 0.00
#define PH_SLOPE  5.3
#define PH_ARRAY_LENGTH 40
int pHArray[PH_ARRAY_LENGTH];
int pHArrayIndex = 0;

// --- DS18B20 SETUP ---
OneWire oneWire(waterTempPin);
DallasTemperature waterTempSensor(&oneWire);

// --- FLOW SENSOR ISR ---
void IRAM_ATTR pulseCounter() {
  pulseCount++;
}

// --- pH averaging (removes min/max outliers) ---
double averageArray(int* arr, int number) {
  int i, maxVal, minVal;
  long amount = 0;
  if (number <= 0) return 0;
  if (number < 5) {
    for (i = 0; i < number; i++) amount += arr[i];
    return (double)amount / number;
  }
  if (arr[0] < arr[1]) { minVal = arr[0]; maxVal = arr[1]; }
  else                 { minVal = arr[1]; maxVal = arr[0]; }
  for (i = 2; i < number; i++) {
    if      (arr[i] < minVal) { amount += minVal; minVal = arr[i]; }
    else if (arr[i] > maxVal) { amount += maxVal; maxVal = arr[i]; }
    else                      { amount += arr[i]; }
  }
  return (double)amount / (number - 2);
}

// --- MH-Z19E UART read (works with any HardwareSerial port) ---
int readCO2(HardwareSerial &port) {
  byte cmd[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
  byte response[9];
  while (port.available() > 0) { port.read(); }
  port.write(cmd, 9);
  unsigned long timeout = millis();
  while (port.available() < 9) {
    if (millis() - timeout > 500) return -1;
    delay(5);
  }
  port.readBytes(response, 9);
  byte crc = 0;
  for (int i = 1; i < 8; i++) { crc += response[i]; }
  crc = 255 - crc; crc++;
  if (crc == response[8]) {
    return (response[2] * 256) + response[3];
  }
  return -1;
}

// --- DASHBOARD HTML ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>AirForest Telemetry</title>
  <style>
    body { font-family: 'Arial', sans-serif; background-color: #121212; color: #ffffff; text-align: center; margin: 0; padding: 20px; }
    h2 { color: #bb86fc; font-size: 22px; margin-bottom: 20px; }
    .card { background-color: #1e1e1e; border-radius: 15px; padding: 18px; margin: 12px auto; width: 90%; max-width: 400px; box-shadow: 0 4px 8px rgba(0,0,0,0.3); }
    .value { font-size: 44px; font-weight: bold; color: #03dac6; margin: 8px 0; }
    .unit { font-size: 16px; color: #aaaaaa; }
    .label { font-size: 14px; text-transform: uppercase; letter-spacing: 1px; color: #888888; }
    .error { color: #cf6679; }
    .voltage { font-size: 12px; color: #666666; margin-top: 4px; }
    .absorb { color: #81c784; }
  </style>
</head>
<body>
  <h2>AirForest Sensor Dashboard</h2>

  <div class="card">
    <div class="label">Air Flow Rate</div>
    <div class="value" id="flowVal">--</div>
    <div class="unit">Liters / Min</div>
  </div>

  <div class="card">
    <div class="label">CO2 Inlet</div>
    <div class="value" id="co2InVal">--</div>
    <div class="unit">PPM (before panel)</div>
  </div>

  <div class="card">
    <div class="label">CO2 Outlet</div>
    <div class="value" id="co2OutVal">--</div>
    <div class="unit">PPM (after panel)</div>
  </div>

  <div class="card">
    <div class="label">CO2 Absorbed</div>
    <div class="value absorb" id="co2AbsVal">--</div>
    <div class="unit">PPM (inlet - outlet)</div>
  </div>

  <div class="card">
    <div class="label">Water Temperature</div>
    <div class="value" id="tempVal">--</div>
    <div class="unit">&deg;C</div>
  </div>

  <div class="card">
    <div class="label">pH Level</div>
    <div class="value" id="phVal">--</div>
    <div class="unit">pH</div>
    <div class="voltage" id="phVolt">--</div>
  </div>

<script>
setInterval(function() {
  fetch('/data')
    .then(response => response.json())
    .then(data => {
      document.getElementById("flowVal").innerText = data.flow.toFixed(2);

      if (data.co2_in > 0)  { document.getElementById("co2InVal").innerText = data.co2_in;  document.getElementById("co2InVal").className = "value"; }
      else                  { document.getElementById("co2InVal").innerText = "WAIT";       document.getElementById("co2InVal").className = "value error"; }

      if (data.co2_out > 0) { document.getElementById("co2OutVal").innerText = data.co2_out; document.getElementById("co2OutVal").className = "value"; }
      else                  { document.getElementById("co2OutVal").innerText = "WAIT";       document.getElementById("co2OutVal").className = "value error"; }

      if (data.co2_in > 0 && data.co2_out > 0) {
        document.getElementById("co2AbsVal").innerText = data.co2_abs;
        document.getElementById("co2AbsVal").className = "value absorb";
      } else {
        document.getElementById("co2AbsVal").innerText = "--";
      }

      document.getElementById("tempVal").innerText = data.temp.toFixed(2);
      document.getElementById("phVal").innerText   = data.ph.toFixed(2);
      document.getElementById("phVolt").innerText  = "Analog V: " + data.phv.toFixed(3) + " V (raw ADC: " + data.phr + ")";
    });
}, 1000);
</script>
</body>
</html>)rawliteral";


void setup() {
  Serial.begin(115200);

  // UART for inlet CO2 (MH-Z19E #1)
  Serial2.begin(9600, SERIAL_8N1, 16, 17);

  // UART for outlet CO2 (MH-Z19E #2) — remapped to free pins
  Serial1.begin(9600, SERIAL_8N1, co2OutletRxPin, co2OutletTxPin);

  // Flow sensor
  pinMode(flowSensorPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(flowSensorPin), pulseCounter, FALLING);

  // pH sensor ADC
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // DS18B20 — non-blocking mode so 750ms conversion doesn't stall loop
  waterTempSensor.begin();
  waterTempSensor.setResolution(12);
  waterTempSensor.setWaitForConversion(false);
  waterTempSensor.requestTemperatures();   // kick off first conversion

  // Wi-Fi AP
  WiFi.softAP(ssid, password);
  Serial.print("Dashboard IP: ");
  Serial.println(WiFi.softAPIP());

  // Web routes
  server.on("/", []() {
    server.send(200, "text/html", index_html);
  });
  server.on("/data", []() {
    String json = "{\"flow\":"    + String(currentFlowRate) +
                  ",\"co2_in\":"  + String(currentCO2_inlet) +
                  ",\"co2_out\":" + String(currentCO2_outlet) +
                  ",\"co2_abs\":" + String(currentCO2_absorbed) +
                  ",\"temp\":"    + String(currentWaterTemp, 2) +
                  ",\"ph\":"      + String(currentPH, 2) +
                  ",\"phv\":"     + String(currentPHVoltage, 3) +
                  ",\"phr\":"     + String((int)averageArray(pHArray, PH_ARRAY_LENGTH)) + "}";
    server.send(200, "application/json", json);
  });
  server.begin();

  Serial.println("System online. Connect to 'Sensor_Dashboard', open 192.168.4.1");
  oldTime = millis();
}


void loop() {
  server.handleClient();

  // --- pH: continuous sampling every 20ms ---
  static unsigned long phSampleTime = millis();
  if (millis() - phSampleTime > 20) {
    pHArray[pHArrayIndex++] = analogRead(phSensorPin);
    if (pHArrayIndex == PH_ARRAY_LENGTH) pHArrayIndex = 0;
    phSampleTime = millis();
  }

  // --- 1-second telemetry cycle ---
  if ((millis() - oldTime) > 1000) {

    // 1. Flow sensor
    detachInterrupt(digitalPinToInterrupt(flowSensorPin));
    currentFlowRate = (float)pulseCount / 7.5;
    pulseCount = 0;
    attachInterrupt(digitalPinToInterrupt(flowSensorPin), pulseCounter, FALLING);

    // 2. CO2 sensors — inlet + outlet
    currentCO2_inlet  = readCO2(Serial2);
    currentCO2_outlet = readCO2(Serial1);
    if (currentCO2_inlet > 0 && currentCO2_outlet > 0) {
      currentCO2_absorbed = currentCO2_inlet - currentCO2_outlet;
    } else {
      currentCO2_absorbed = 0;
    }

    // 3. pH sensor (3.3V powered, direct read)
    float avgRaw = averageArray(pHArray, PH_ARRAY_LENGTH);
    currentPHVoltage = avgRaw * 3.3 / 4095.0;
    currentPH = PH_SLOPE * currentPHVoltage + PH_OFFSET;

    // 4. Water temperature (non-blocking: read previous result, queue next)
    float t = waterTempSensor.getTempCByIndex(0);
    if (t != DEVICE_DISCONNECTED_C && t > -50.0) {
      currentWaterTemp = t;
    }
    waterTempSensor.requestTemperatures();   // kick off next conversion

    // 5. Serial output
    Serial.print("Flow: ");    Serial.print(currentFlowRate, 2); Serial.print(" L/min | ");
    Serial.print("CO2in: ");
    if (currentCO2_inlet > 0)  Serial.print(currentCO2_inlet);  else Serial.print("wait");
    Serial.print(" | CO2out: ");
    if (currentCO2_outlet > 0) Serial.print(currentCO2_outlet); else Serial.print("wait");
    Serial.print(" | Abs: "); Serial.print(currentCO2_absorbed); Serial.print(" ppm");
    Serial.print(" | Temp: "); Serial.print(currentWaterTemp, 2); Serial.print(" C");
    Serial.print(" | pH: ");   Serial.print(currentPH, 2);
    Serial.print(" (V=");      Serial.print(currentPHVoltage, 3);
    Serial.print(", raw=");    Serial.print((int)avgRaw);
    Serial.println(")");

    oldTime = millis();
  }
}
