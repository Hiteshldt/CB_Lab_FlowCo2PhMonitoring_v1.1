#include <WiFi.h>
#include <WebServer.h>

// --- PIN DEFINITIONS ---
const int flowSensorPin = 27;
const int phSensorPin = 32;       // Analog input for SEN0169 (direct, no divider at 3.3V)
// MH-Z19E uses GPIO 16 (RX2) and GPIO 17 (TX2) via Serial2

// --- WI-FI ACCESS POINT SETTINGS ---
const char* ssid = "Sensor_Dashboard";
const char* password = "password123";

WebServer server(80);

// --- GLOBAL SENSOR DATA ---
volatile int pulseCount = 0;
float currentFlowRate = 0.0;
int currentCO2 = 0;
float currentPH = 0.0;
float currentPHVoltage = 0.0;
unsigned long oldTime = 0;

// --- pH CALIBRATION (3.3V POWER) ---
// These are STARTING estimates. You MUST calibrate with buffer solutions.
// Step 1: Dip in pH 7.00 buffer, note the voltage -> adjust PH_OFFSET so it reads 7.00
// Step 2: Dip in pH 4.00 buffer, adjust the board's gain pot OR tweak PH_SLOPE
#define PH_OFFSET 0.00
#define PH_SLOPE  5.3            // ~3.5 * (5.0/3.3) for 3.3V operation

#define PH_ARRAY_LENGTH 40
int pHArray[PH_ARRAY_LENGTH];
int pHArrayIndex = 0;

// --- FLOW SENSOR ISR ---
void IRAM_ATTR pulseCounter() {
  pulseCount++;
}

// --- pH averaging (removes min/max outliers) ---
double averageArray(int* arr, int number) {
  int i;
  int maxVal, minVal;
  double avg;
  long amount = 0;
  if (number <= 0) return 0;
  if (number < 5) {
    for (i = 0; i < number; i++) amount += arr[i];
    avg = amount / number;
    return avg;
  } else {
    if (arr[0] < arr[1]) { minVal = arr[0]; maxVal = arr[1]; }
    else { minVal = arr[1]; maxVal = arr[0]; }
    for (i = 2; i < number; i++) {
      if (arr[i] < minVal) { amount += minVal; minVal = arr[i]; }
      else if (arr[i] > maxVal) { amount += maxVal; maxVal = arr[i]; }
      else { amount += arr[i]; }
    }
    avg = (double)amount / (number - 2);
  }
  return avg;
}

// --- MH-Z19E read function ---
int readCO2() {
  byte cmd[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
  byte response[9];
  while (Serial2.available() > 0) { Serial2.read(); }
  Serial2.write(cmd, 9);
  unsigned long timeout = millis();
  while (Serial2.available() < 9) {
    if (millis() - timeout > 500) return -1;
    delay(5);
  }
  Serial2.readBytes(response, 9);
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
    <div class="label">CO2 Concentration</div>
    <div class="value" id="co2Val">--</div>
    <div class="unit">PPM</div>
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
      if(data.co2 > 0) {
        document.getElementById("co2Val").innerText = data.co2;
        document.getElementById("co2Val").className = "value";
      } else {
        document.getElementById("co2Val").innerText = "WAIT";
        document.getElementById("co2Val").className = "value error";
      }
      document.getElementById("phVal").innerText = data.ph.toFixed(2);
      document.getElementById("phVolt").innerText = "Analog V: " + data.phv.toFixed(3) + " V (raw ADC: " + data.phr + ")";
    });
}, 1000);
</script>
</body>
</html>)rawliteral";


void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);

  // Flow sensor
  pinMode(flowSensorPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(flowSensorPin), pulseCounter, FALLING);

  // pH sensor — GPIO 32 is ADC1_CH4, safe to use alongside WiFi
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);  // Full 0-3.3V range on all ADC pins

  // Wi-Fi AP
  WiFi.softAP(ssid, password);
  Serial.print("Dashboard IP: ");
  Serial.println(WiFi.softAPIP());

  // Web routes
  server.on("/", []() {
    server.send(200, "text/html", index_html);
  });
  server.on("/data", []() {
    String json = "{\"flow\":" + String(currentFlowRate) +
                  ",\"co2\":" + String(currentCO2) +
                  ",\"ph\":" + String(currentPH, 2) +
                  ",\"phv\":" + String(currentPHVoltage, 3) +
                  ",\"phr\":" + String((int)averageArray(pHArray, PH_ARRAY_LENGTH)) + "}";
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

    // 2. CO2 sensor
    currentCO2 = readCO2();

    // 3. pH sensor (3.3V powered, direct read)
    float avgRaw = averageArray(pHArray, PH_ARRAY_LENGTH);
    currentPHVoltage = avgRaw * 3.3 / 4095.0;
    currentPH = PH_SLOPE * currentPHVoltage + PH_OFFSET;

    // 4. Serial output
    Serial.print("Flow: "); Serial.print(currentFlowRate, 2); Serial.print(" L/min | ");
    if (currentCO2 > 0) { Serial.print("CO2: "); Serial.print(currentCO2); Serial.print(" ppm | "); }
    else { Serial.print("CO2: waiting | "); }
    Serial.print("pH: "); Serial.print(currentPH, 2);
    Serial.print(" (V="); Serial.print(currentPHVoltage, 3);
    Serial.print(", raw="); Serial.print((int)avgRaw);
    Serial.println(")");

    oldTime = millis();
  }
}