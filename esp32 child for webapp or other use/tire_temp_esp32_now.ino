/*

    traviscea DIY ESP32 Thermal Tire Temperature – Version 1.0
    Copyright (c) 2026 Travis Way
    ------------------------------------------------

    FEATURES
    ------------------------------------------------
    - MLX90640 thermal camera
    - Calibration/debug mode
    - Runtime ESP-NOW mode
    - Automatic tire detection
    - Ellipse tire masking
    - Perspective-compensated 6 zones
    - EMA smoothing
    - Brake hotspot rejection
    - Low power runtime mode

    MODES
    ------------------------------------------------
    GPIO 0 LOW  = Calibration Web Viewer
    GPIO 0 HIGH = ESP-NOW Runtime

    WIRING
    ------------------------------------------------
    MLX90640:
        SDA -> GPIO 23
        SCL -> GPIO 19

    Calibration Jumper:
        GPIO0 -> GND

    REQUIRED LIBRARIES
    ------------------------------------------------
    - Adafruit MLX90640
    - ESPAsyncWebServer
    - AsyncTCP
*/

#include <WiFi.h>
#include <esp_now.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include <Wire.h>
#include <Adafruit_MLX90640.h>

#include <Preferences.h>

// =====================================================
// CONFIG
// =====================================================

#define MLX_SDA 23
#define MLX_SCL 19

#define CAL_PIN 13
// CHANGE THIS FOR EACH WHEEL
// 0 = FL, 1 = FR, 2 = RL, 3 = RR
#define TIRE_ID 3

#define DEBUG_MODE false

#define BATTERY_PIN 34


#define MAX_ALLOWED_TIRE_TEMP_F 230

int smoothedBattery = 100;

uint32_t lastBatteryRead = 0;


// =====================================================
// ESP-NOW RECEIVER MAC BELOW
// Ex mac address = 11:53:CB:8F:89:49
// =====================================================
uint8_t receiverAddress[] = {
  0x11, 0x53, 0xCB,
  0x8F, 0x89, 0x49
};

// =====================================================
// WIFI AP
// =====================================================

const char *ssid =
  "TIRE-CAL-1";

const char *password =
  "12345678";

// =====================================================
// WEB SERVER
// =====================================================

AsyncWebServer server(80);

AsyncWebSocket ws("/ws");

// =====================================================
// MLX90640
// =====================================================

Adafruit_MLX90640 mlx;

float frame[32 * 24];

bool mlxConnected = false;

// =====================================================
// MODE
// =====================================================

bool calibrationMode = false;

// =====================================================
// EMA FILTERING
// =====================================================

float smoothedTemps[5] = {
  0, 0, 0, 0, 0
};

bool firstFrame = true;


Preferences prefs;

int savedMinX = 4;
int savedMaxX = 28;

int savedMinY = 5;
int savedMaxY = 19;


float savedBands[4] = {
  0.18,
  0.38,
  0.62,
  0.82
};

// =====================================================
// DEBUG IMAGE
// =====================================================

uint8_t frameBytes[786];

// =====================================================
// ESP-NOW PACKET
// =====================================================

typedef struct __attribute__((packed)) {
  uint8_t tireID;
  uint8_t battery;
  int16_t zone[5];
  int16_t hottest;
} TirePacket;

TirePacket packet;

volatile uint32_t sendOk = 0;
volatile uint32_t sendFail = 0;

static int stableFrames = 0;

// =====================================================
// TEMP CONVERSION
// =====================================================

float cToF(float c) {
  return (c * 9.0f / 5.0f) + 32.0f;
}


int voltageToPercent(float v) {
  const float volts[] = {
    3.30,
    3.60,
    3.70,
    3.80,
    3.90,
    4.00,
    4.05,
    4.11
  };

  const int percents[] = {
    0,
    10,
    20,
    40,
    60,
    80,
    90,
    100
  };

  const int count =
    sizeof(volts)
    / sizeof(volts[0]);

  if (v <= volts[0])
    return percents[0];

  if (v >= volts[count - 1])
    return percents[count - 1];

  for (int i = 0; i < count - 1; i++) {
    if (v >= volts[i] && v <= volts[i + 1]) {
      float t =
        (v - volts[i])
        / (volts[i + 1] - volts[i]);

      return percents[i]
             + (percents[i + 1]
                - percents[i])
                 * t;
    }
  }

  return 0;
}


float readBatteryPercent() {

  uint32_t total = 0;

  for (int i = 0; i < 32; i++) {
    total += analogRead(BATTERY_PIN);
  }

  int rawBat = total / 32;

  float voltage =
    (rawBat / 4095.0f)
    * 3.3f
    * 2.0f
    * 1.073f;

  if (DEBUG_MODE) {
    Serial.print("Battery Voltage: ");
    Serial.println(voltage, 3);
  }

  int p = voltageToPercent(voltage);

  if (p > 100) p = 100;
  if (p < 0) p = 0;

  // EMA smoothing
  smoothedBattery =
    (smoothedBattery * 0.75f)
    + (p * 0.25f);

  return smoothedBattery;
}

// =====================================================
// HTML
// =====================================================

const char index_html[] PROGMEM = R"rawliteral(

<!DOCTYPE html>
<html>

<head>

<meta charset="utf-8">

<title>Tire Calibration</title>

<style>

body {

    margin:0;
    background:black;
    overflow:hidden;

    display:flex;
    flex-direction:column;

    justify-content:center;
    align-items:center;

    height:100vh;

    font-family:Arial;
    color:white;
}

canvas {
    image-rendering:pixelated;

    width:95vmin;
    height:75vmin;

    border:2px solid #444;
}

.info {

    margin-top:10px;

    display:flex;
    gap:25px;

    font-size:24px;
    font-weight:bold;
}

</style>

</head>

<body>

<canvas
    id="thermal"
    width="32"
    height="24">
</canvas>

<div class="info">

<div>
HIGH:
<span id="high">0</span>F
</div>

<div>
LOW:
<span id="low">0</span>F
</div>

<div>
BAT:
<span id="battery">0</span>
</div>

</div>

</div>

<div class="info">

<button onclick="saveCal()">
SAVE
</button>

</div>

<script>

const canvas =
    document.getElementById("thermal");

const ctx =
    canvas.getContext("2d");

ctx.imageSmoothingEnabled = false;

const socket =
    new WebSocket(
        `ws://${location.host}/ws`
    );

socket.binaryType =
    "arraybuffer";

const image =
    ctx.createImageData(32,24);

// =========================================
// REGION
// =========================================

let selMinX = 4;
let selMaxX = 28;

let selMinY = 5;
let selMaxY = 19;

// =========================================
// CUSTOM BAND POSITIONS
// =========================================

let bandLines =
[
    0.18,
    0.38,
    0.62,
    0.82
];

// =========================================
// DRAGGING
// =========================================

let dragging = false;

let draggingBand = -1;

let startX = 0;
let startY = 0;

// =========================================
// HELPERS
// =========================================

function getCanvasPos(clientX, clientY)
{
    const rect =
        canvas.getBoundingClientRect();

    const scaleX =
        32 / rect.width;

    const scaleY =
        24 / rect.height;

    return {
        x:
            Math.floor(
                (clientX - rect.left)
                * scaleX
            ),

        y:
            Math.floor(
                (clientY - rect.top)
                * scaleY
            )
    };
}

// =========================================
// START DRAG
// =========================================

function startDrag(x,y)
{
    startX = x;
    startY = y;

    draggingBand = -1;

    // -------------------------------------
    // CHECK BAND DRAG
    // -------------------------------------

    for(let i=0;i<bandLines.length;i++)
    {
        let bandX =
            selMinX
            +
            (
                (selMaxX - selMinX)
                *
                bandLines[i]
            );

        if(
            Math.abs(x - bandX)
            < 2
        )
        {
            draggingBand = i;
            break;
        }
    }

    dragging = true;
}

// =========================================
// MOVE DRAG
// =========================================

function moveDrag(x,y)
{
    if(!dragging)
        return;

    // -------------------------------------
    // MOVE BAND
    // -------------------------------------

    if(draggingBand >= 0)
    {
        let normalized =
            (
                x - selMinX
            )
            /
            (
                selMaxX - selMinX
            );

        normalized =
            Math.max(
                0.05,
                Math.min(
                    0.95,
                    normalized
                )
            );

        bandLines[draggingBand] =
            normalized;

        // Sort so lines never cross

        bandLines.sort(
            (a,b)=>a-b
        );

        return;
    }

    // -------------------------------------
    // MOVE BOX
    // -------------------------------------

    selMinX =
        Math.min(startX,x);

    selMaxX =
        Math.max(startX,x);

    selMinY =
        Math.min(startY,y);

    selMaxY =
        Math.max(startY,y);
}

// =========================================
// END DRAG
// =========================================

function endDrag()
{
    dragging = false;
    draggingBand = -1;
}

// =========================================
// MOUSE
// =========================================

canvas.addEventListener(
    "mousedown",
    (e)=>
{
    const pos =
        getCanvasPos(
            e.clientX,
            e.clientY
        );

    startDrag(
        pos.x,
        pos.y
    );
});

canvas.addEventListener(
    "mousemove",
    (e)=>
{
    const pos =
        getCanvasPos(
            e.clientX,
            e.clientY
        );

    moveDrag(
        pos.x,
        pos.y
    );
});

canvas.addEventListener(
    "mouseup",
    endDrag
);

// =========================================
// TOUCH
// =========================================

canvas.addEventListener(
    "touchstart",
    (e)=>
{
    e.preventDefault();

    const touch =
        e.touches[0];

    const pos =
        getCanvasPos(
            touch.clientX,
            touch.clientY
        );

    startDrag(
        pos.x,
        pos.y
    );
});

canvas.addEventListener(
    "touchmove",
    (e)=>
{
    e.preventDefault();

    const touch =
        e.touches[0];

    const pos =
        getCanvasPos(
            touch.clientX,
            touch.clientY
        );

    moveDrag(
        pos.x,
        pos.y
    );
});

canvas.addEventListener(
    "touchend",
    endDrag
);

// =========================================
// WEBSOCKET FRAME
// =========================================

socket.onmessage = (event)=>
{
    const buffer =
        new Uint8Array(event.data);

    // =====================================
    // THERMAL IMAGE
    // =====================================

    for(let i=0;i<768;i++)
    {
        const val =
            buffer[i];

        const idx =
            i * 4;

        let r=0,g=0,b=0;

        if(val < 64)
        {
            r = 0;
            g = val * 4;
            b = 255;
        }
        else if(val < 128)
        {
            r = 0;
            g = 255;
            b = 255 - ((val - 64) * 4);
        }
        else if(val < 192)
        {
            r = (val - 128) * 4;
            g = 255;
            b = 0;
        }
        else
        {
            r = 255;
            g = 255 - ((val - 192) * 4);
            b = 0;
        }

        image.data[idx+0]=r;
        image.data[idx+1]=g;
        image.data[idx+2]=b;
        image.data[idx+3]=255;
    }

    ctx.putImageData(image,0,0);

    // =====================================
    // LOAD SAVED REGION ONCE
    // =====================================

    const minX =
        buffer[772];

    const maxX =
        buffer[773];

    const minY =
        buffer[774];

    const maxY =
        buffer[775];

    if(!window.loadedOnce)
    {
        selMinX = minX;
        selMaxX = maxX;

        selMinY = minY;
        selMaxY = maxY;

        bandLines =
        [
            buffer[779] / 100.0,
            buffer[780] / 100.0,
            buffer[781] / 100.0,
            buffer[782] / 100.0
        ];

        window.loadedOnce = true;
    }

    // =====================================
    // HOT TEMP
    // =====================================

    const high =
        (
            buffer[777]
            |
            (buffer[778] << 8)
        ) / 10;

    const low =
        (
            buffer[784]
            |
            (buffer[785] << 8)
        ) / 10.0;

    document.getElementById(
        "high"
    ).innerText =
        high.toFixed(1);
    document.getElementById(
        "low"
    ).innerText =
        low.toFixed(1);
    
    const batteryPercent =
        buffer[783];

    document.getElementById(
        "battery"
    ).innerText =
        batteryPercent + "%";
    

    // =====================================
    // RED SELECTION OUTLINE ONLY
    // =====================================

    ctx.strokeStyle =
    "#cc0000";

    ctx.lineWidth =
        1;

    ctx.translate(0.5,0.5);

    ctx.strokeRect(
        selMinX,
        selMinY,
        selMaxX - selMinX,
        selMaxY - selMinY
    );

    // =====================================
    // SINGLE SOLID YELLOW BAND LINES
    // =====================================

    for(let i=0;i<bandLines.length;i++)
    {
        let x =
            selMinX
            +
            (
                (selMaxX - selMinX)
                *
                bandLines[i]
            );

        ctx.beginPath();

        ctx.moveTo(
            x,
            selMinY
        );

        ctx.lineTo(
            x,
            selMaxY
        );

        ctx.strokeStyle =
            "#cc0000";

        ctx.lineWidth =
            1;

        ctx.stroke();
    }

    ctx.setTransform(1,0,0,1,0,0);


};

// =========================================
// SAVE
// =========================================

async function saveCal()
{
    await fetch(
        `/save?minX=${selMinX}&maxX=${selMaxX}&minY=${selMinY}&maxY=${selMaxY}&bands=${bandLines.join(",")}`
    );

    alert("Saved");
}

</script>

</body>
</html>

)rawliteral";

// =====================================================
// WEBSOCKET
// =====================================================

void onWebSocketEvent(
  AsyncWebSocket *server,
  AsyncWebSocketClient *client,
  AwsEventType type,
  void *arg,
  uint8_t *data,
  size_t len) {
}

// =====================================================
// ESP-NOW CALLBACK
// =====================================================

void onDataSent(
  const wifi_tx_info_t *info,
  esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    sendOk++;

    if (DEBUG_MODE) {
      Serial.println(
        "ESP-NOW SEND OK");
    }
  } else {
    sendFail++;

    Serial.println(
      "ESP-NOW SEND FAIL");
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  setCpuFrequencyMhz(80);

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("======================");
  Serial.println("BOOTING");
  Serial.println("======================");

  pinMode(
    CAL_PIN,
    INPUT_PULLUP);

  pinMode(BATTERY_PIN, INPUT);

  delay(50);

  // =============================================
  // DEBUG CAL PIN
  // =============================================

  int calState =
    digitalRead(CAL_PIN);

  Serial.println();
  Serial.println("======================");
  Serial.println("CAL PIN DEBUG");
  Serial.println("======================");

  Serial.print("CAL_PIN: ");
  Serial.println(CAL_PIN);

  Serial.print("PIN STATE: ");

  if (calState == LOW) {
    Serial.println("LOW");
  } else {
    Serial.println("HIGH");
  }

  // =============================================
  // MODE SELECT
  // =============================================

  calibrationMode =
    (calState == LOW);

  if (calibrationMode) {
    Serial.println("MODE: CALIBRATION");
  } else {
    Serial.println("MODE: ESP-NOW RUNTIME");
  }

  Serial.println("======================");
  // =============================================
  // I2C
  // =============================================

  // =============================================
  // LOAD SAVED CALIBRATION
  // =============================================

  prefs.begin(
    "tirecal",
    false);

  savedMinX =
    prefs.getInt(
      "minX",
      4);

  savedMaxX =
    prefs.getInt(
      "maxX",
      28);

  savedMinY =
    prefs.getInt(
      "minY",
      5);

  savedMaxY =
    prefs.getInt(
      "maxY",
      19);

  String bandString =
    prefs.getString(
      "bands",
      "0.15,0.32,0.50,0.68,0.85");

  sscanf(
    bandString.c_str(),
    "%f,%f,%f,%f",
    &savedBands[0],
    &savedBands[1],
    &savedBands[2],
    &savedBands[3]);

  Serial.println("Loaded Calibration");

  Serial.print("X: ");
  Serial.print(savedMinX);
  Serial.print(" -> ");
  Serial.println(savedMaxX);

  Serial.print("Y: ");
  Serial.print(savedMinY);
  Serial.print(" -> ");
  Serial.println(savedMaxY);


  Wire.begin(
    MLX_SDA,
    MLX_SCL);

  Wire.setClock(1000000);

  // =============================================
  // MLX
  // =============================================

  mlxConnected =
    mlx.begin(
      MLX90640_I2CADDR_DEFAULT,
      &Wire);

  if (!mlxConnected) {
    Serial.println(
      "MLX FAIL");

    while (true)
      ;
  }

  mlx.setMode(
    MLX90640_CHESS);

  mlx.setResolution(
    MLX90640_ADC_18BIT);

  mlx.setRefreshRate(
    MLX90640_16_HZ);

  // =============================================
  // CALIBRATION MODE
  // =============================================

  if (calibrationMode) {
    Serial.println(
      "CAL MODE");

    WiFi.mode(WIFI_MODE_AP);

    WiFi.softAP(
      ssid,
      password);

    Serial.println(
      WiFi.softAPIP());

    ws.onEvent(
      onWebSocketEvent);

    server.addHandler(&ws);

    // =============================================
    // SAVE CALIBRATION
    // =============================================

    server.on(
      "/save",
      HTTP_GET,
      [](AsyncWebServerRequest *request) {
        if (
          request->hasParam("minX")
          && request->hasParam("maxX")
          && request->hasParam("minY")
          && request->hasParam("maxY")) {
          savedMinX =
            request->getParam("minX")->value().toInt();

          savedMaxX =
            request->getParam("maxX")->value().toInt();

          savedMinY =
            request->getParam("minY")->value().toInt();

          savedMaxY =
            request->getParam("maxY")->value().toInt();

          prefs.putInt(
            "minX",
            savedMinX);

          prefs.putInt(
            "maxX",
            savedMaxX);

          prefs.putInt(
            "minY",
            savedMinY);

          prefs.putInt(
            "maxY",
            savedMaxY);

          String bandString =
            request->getParam("bands")->value();

          sscanf(
            bandString.c_str(),
            "%f,%f,%f,%f",
            &savedBands[0],
            &savedBands[1],
            &savedBands[2],
            &savedBands[3]);

          prefs.putString(
            "bands",
            bandString);

          Serial.println();
          Serial.println("CALIBRATION SAVED");

          request->send(
            200,
            "text/plain",
            "saved");

          if (DEBUG_MODE) {
            Serial.println();
            Serial.println("======================");
            Serial.println("CALIBRATION SAVED");
            Serial.println("======================");

            Serial.print("X RANGE: ");
            Serial.print(savedMinX);
            Serial.print(" -> ");
            Serial.println(savedMaxX);

            Serial.print("Y RANGE: ");
            Serial.print(savedMinY);
            Serial.print(" -> ");
            Serial.println(savedMaxY);

            Serial.println();

            Serial.println("BANDS:");

            for (int i = 0; i < 4; i++) {
              Serial.print("Band ");

              Serial.print(i);

              Serial.print(": ");

              Serial.println(
                savedBands[i],
                3);
            }
          }

          Serial.println("======================");
        } else {
          request->send(
            400,
            "text/plain",
            "missing params");
        }
      });

    server.on(
      "/",
      HTTP_GET,
      [](AsyncWebServerRequest *request) {
        request->send_P(
          200,
          "text/html",
          index_html);
      });

    server.begin();
  }

  // =============================================
  // RUNTIME MODE
  // =============================================

  else {
    Serial.println(
      "ESP-NOW MODE");

    WiFi.mode(WIFI_STA);

    if (
      esp_now_init()
      != ESP_OK) {
      while (true)
        ;
    }

    esp_now_register_send_cb(
      onDataSent);

    esp_now_peer_info_t peerInfo = {};

    memcpy(
      peerInfo.peer_addr,
      receiverAddress,
      6);

    peerInfo.channel = 0;

    peerInfo.encrypt = false;

    esp_now_add_peer(
      &peerInfo);
  }
}

// =====================================================
// LOOP
// =====================================================

void loop() {

  if (
    mlx.getFrame(frame)
    != 0) {
    return;
  }

  // =============================================
  // GLOBAL TEMPS
  // =============================================

  float minTemp = 999;
  float maxTemp = -999;

  for (int i = 0; i < 768; i++) {
    float t =
      cToF(frame[i]);

    if (t < minTemp)
      minTemp = t;

    if (t > maxTemp)
      maxTemp = t;
  }

  // =============================================
  // AMBIENT
  // =============================================

  float ambientSum = 0;

  int ambientCount = 0;

  for (int y = 0; y < 24; y++) {
    for (int x = 0; x < 32; x++) {
      if (
        x < 3 || x > 28 || y < 3 || y > 20) {
        ambientSum +=
          cToF(
            frame[(y * 32) + x]);

        ambientCount++;
      }
    }
  }

  float ambient =
    ambientSum
    / ambientCount;

  float tireThreshold =
    ambient + 8.0f;

  // =============================================
  // FIND TIRE REGION
  // =============================================

  int minX = savedMinX;

  int maxX = savedMaxX;

  int minY = savedMinY;

  int maxY = savedMaxY;

  int validPixels = 0;

  // =============================================
  // ELLIPSE
  // =============================================

  float cx =
    (minX + maxX) * 0.5f;

  float cy =
    (minY + maxY) * 0.5f;

  float rx =
    (maxX - minX) * 0.85f;

  float ry =
    (maxY - minY) * 0.85f;

  // =============================================
  // ZONES
  // =============================================

  float sums[5] = {
    0, 0, 0, 0, 0
  };

  int counts[5] = {
    0, 0, 0, 0, 0
  };

  float hottest =
    tireThreshold;

  float regionWidth =
    (float)(maxX - minX);

  // =============================================
  // PROCESS PIXELS
  // =============================================

  for (int y = minY; y <= maxY; y++) {
    for (int x = minX; x <= maxX; x++) {
      int idx =
        (y * 32) + x;

      float tempF =
        cToF(frame[idx]);

      float dx =
        (x - cx) / rx;

      float dy =
        (y - cy) / ry;

      float ellipse =
        (dx * dx)
        + (dy * dy);

      if (ellipse > 1.0f)
        continue;

      if (tempF > 250)
        continue;

      if (!calibrationMode && tempF < (ambient + 3.0f))
        continue;

      if (tempF > hottest)
        hottest = tempF;

      float normalizedX =
        (float)(x - minX)
        / regionWidth;

      int relativeX = 0;

      if (normalizedX < savedBands[0])
        relativeX = 0;

      else if (normalizedX < savedBands[1])
        relativeX = 1;

      else if (normalizedX < savedBands[2])
        relativeX = 2;

      else if (normalizedX < savedBands[3])
        relativeX = 3;

      else
        relativeX = 4;

      if (tempF > MAX_ALLOWED_TIRE_TEMP_F)
        continue;

      sums[relativeX] +=
        tempF;

      counts[relativeX]++;
    }
  }

  // =============================================
  // COMPUTE ZONES
  // =============================================

  for (int i = 0; i < 5; i++) {
    // =====================================
    // NO PIXELS IN ZONE
    // =====================================

    if (counts[i] <= 0) {
      smoothedTemps[i] =
        (ambient * 0.10f)
        + (smoothedTemps[i]
           * 0.90f);

      if (smoothedTemps[i] < 40) {
        smoothedTemps[i] =
          ambient;
      }
    }

    // =====================================
    // VALID PIXELS
    // =====================================

    else {
      float current =
        sums[i]
        / counts[i];

      if (firstFrame) {
        smoothedTemps[i] =
          current;
      } else {
        smoothedTemps[i] =
          (current * 0.15f)
          + (smoothedTemps[i]
             * 0.85f);
      }
    }

    if (
      isnan(smoothedTemps[i])
      || smoothedTemps[i] < 50.0f
      || smoothedTemps[i] > 250.0f) {
      smoothedTemps[i] =
        ambient;
    }

    // =====================================
    // MIRROR RIGHT SIDE TIRES
    // =====================================

    int outputZone = i;

    if (
      TIRE_ID == 1
      || TIRE_ID == 3) {
      outputZone =
        4 - i;
    }

    packet.zone[outputZone] =
      smoothedTemps[i]
      * 10.0f;
  }

  firstFrame = false;
  if (stableFrames < 50) {
    stableFrames++;
    return;
  }

  packet.tireID = TIRE_ID;

  packet.battery = readBatteryPercent();

  packet.hottest =
    hottest * 10.0f;

  // =============================================
  // CALIBRATION VIEWER
  // =============================================

  if (calibrationMode) {
    for (int i = 0; i < 768; i++) {
      float tempF =
        cToF(frame[i]);

      float normalized =
        (tempF - minTemp)
        / (maxTemp - minTemp);

      normalized =
        constrain(
          normalized,
          0,
          1);

      frameBytes[i] =
        normalized * 255;
    }

    frameBytes[772] = minX;
    frameBytes[773] = maxX;

    frameBytes[774] = minY;
    frameBytes[775] = maxY;

    for (int i = 0; i < 4; i++) {
      frameBytes[779 + i] =
        savedBands[i] * 100.0f;
    }

    frameBytes[777] =
      packet.hottest & 0xFF;

    frameBytes[778] =
      (packet.hottest >> 8)
      & 0xFF;

    int16_t lowTemp =
      minTemp * 10.0f;

    static int batteryPercent = 100;

    if (millis() - lastBatteryRead > 3000) {
      batteryPercent =
        readBatteryPercent();

      lastBatteryRead =
        millis();
    }

    frameBytes[783] =
      batteryPercent;

    frameBytes[784] =
      lowTemp & 0xFF;

    frameBytes[785] =
      (lowTemp >> 8)
      & 0xFF;

    frameBytes[786] =
      (lowTemp >> 8) & 0xFF;

    ws.binaryAll(
      frameBytes,
      786);

    ws.cleanupClients();
  }

  // =============================================
  // ESP-NOW RUNTIME
  // =============================================

  else {

    esp_now_send(
      receiverAddress,
      (uint8_t *)&packet,
      sizeof(packet));
    if (DEBUG_MODE) {
      Serial.println();
      Serial.println("======================");
      Serial.println("ESP-NOW SEND");
      Serial.println("======================");

      for (int i = 0; i < 5; i++) {
        Serial.print("ZONE ");

        Serial.print(i);

        Serial.print(": ");

        Serial.print(
          packet.zone[i] / 10.0f,
          1);

        Serial.println("F");
      }

      Serial.print("HOTTEST: ");

      Serial.print(
        packet.hottest / 10.0f,
        1);

      Serial.println("F");

      Serial.println("======================");

      Serial.print("SEND OK: ");
      Serial.println(sendOk);

      Serial.print("SEND FAIL: ");
      Serial.println(sendFail);

      Serial.println();
    }
  }

  delay(5);
}
