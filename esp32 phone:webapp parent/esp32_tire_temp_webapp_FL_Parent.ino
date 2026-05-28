/*

traviscea DIY ESP32 Thermal Tire Temperature – Version 1.0
Copyright (c) 2026 Travis Way


=====================================================
FL MASTER NODE
=====================================================

FEATURES
-----------------------------------------------------
- FL thermal camera node
- Receives FR/RL/RR ESP-NOW packets
- Hosts phone dashboard webpage
- Hosts calibration webpage
- 4 tire live dashboard
- LVGL-style thermal widgets

GPIO0 LOW  = Calibration Mode
GPIO0 HIGH = Runtime Dashboard Mode

=====================================================

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
#define BATTERY_PIN 34

#define TIRE_ID 0

#define MAX_ALLOWED_TIRE_TEMP_F 230

#define DEBUG_MODE false

// =====================================================
// WIFI
// =====================================================

const char *runtimeSSID = "TIRE-DASH";
const char *runtimePassword = "12345678";

const char *calSSID = "TIRE-CAL";
const char *calPassword = "12345678";

// =====================================================
// WEB
// =====================================================

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// =====================================================
// MLX
// =====================================================

Adafruit_MLX90640 mlx;
float frame[32 * 24];

// =====================================================
// MODES
// =====================================================

bool calibrationMode = false;

// =====================================================
// BATTERY
// =====================================================

int smoothedBattery = 100;

uint32_t lastBatteryRead = 0;

// =====================================================
// TEMP FILTERING
// =====================================================

float smoothedTemps[5] = { 0, 0, 0, 0, 0 };
bool firstFrame = true;

// =====================================================
// PREFERENCES
// =====================================================

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
// PACKET STRUCT
// =====================================================

typedef struct __attribute__((packed)) {
  uint8_t tireID;
  uint8_t battery;
  int16_t zone[5];
  int16_t hottest;
} TirePacket;

// =====================================================
// ALL TIRES
// =====================================================

TirePacket tirePackets[4];

uint32_t tireLastSeen[4] = {
  0, 0, 0, 0
};

// =====================================================
// DEBUG FRAME
// =====================================================

uint8_t frameBytes[786];

// =====================================================
// HELPERS
// =====================================================

float cToF(float c) {
  return (c * 9.0f / 5.0f) + 32.0f;
}

// =====================================================
// BATTERY PERCENT
// =====================================================

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
    return 0;

  if (v >= volts[count - 1])
    return 100;

  for (int i = 0; i < count - 1; i++) {
    if (v >= volts[i] && v <= volts[i + 1]) {
      float t =
        (v - volts[i])
        / (volts[i + 1] - volts[i]);

      return percents[i] + ((percents[i + 1] - percents[i]) * t);
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

  int p = voltageToPercent(voltage);

  if (p > 100) p = 100;
  if (p < 0) p = 0;

  smoothedBattery =
    (smoothedBattery * 0.75f)
    + (p * 0.25f);

  return smoothedBattery;
}

// =====================================================
// CALIBRATION HTML
// =====================================================

const char calibration_html[] PROGMEM = R"rawliteral(
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
// DASHBOARD HTML
// =====================================================

const char runtime_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">

<style>

body {

    margin:0;
    background:black;
    color:white;
    font-family:Arial;

    width:100vw;
    height:100vh;

    overflow:hidden;
}

.title {

    width:calc(100% - 20px);

    margin:10px auto 6px auto;

    padding:12px 0;

    text-align:center;

    font-size:14px;
    font-weight:900;

    letter-spacing:2px;

    color:white;

    background:#101010;

    border:2px solid #222;

    border-radius:18px;

    box-shadow:
        inset 0 0 10px rgba(255,255,255,0.03),
        0 0 12px rgba(0,0,0,0.5);

    text-shadow:
        0 0 12px rgba(255,255,255,0.12);
}
canvas {
    image-rendering:pixelated;

    width:95vmin;
    height:75vmin;

    border:2px solid #444;
}

.container {

    width:100%;
    height:calc(100% - 70px);

    display:grid;

    grid-template-columns:
        1fr
        1fr;

    grid-template-areas:
        "fl fr"
        "rl rr"
        "legend legend";

    gap:10px;

    padding:10px;

    box-sizing:border-box;

    align-content:start;
}

.topLeft {
    grid-area:fl;
}

.topRight {
    grid-area:fr;
}

.bottomLeft {
    grid-area:rl;
}

.bottomRight {
    grid-area:rr;
}

.legend {

    grid-area:legend;

    display:flex;
    justify-content:center;
    align-items:center;

    position:relative;

    width:100%;

    margin-top:0;
}

.legendBar {

    width:80vw;
    max-width:420px;

    height:28px;

    border-radius:20px;

    background:
        linear-gradient(
            to right,

            rgb(0,0,255) 0%,
            rgb(0,255,255) 25%,
            rgb(0,255,0) 50%,
            rgb(255,255,0) 75%,
            rgb(255,0,0) 100%
        );
}

.legendLabels {

    position:absolute;

    width:80vw;
    max-width:420px;

    display:flex;
    justify-content:space-between;

    margin-top:52px;

    font-size:22px;
    font-weight:bold;

    color:white;
}

.tire {

    background:#101010;

    border:2px solid #222;

    border-radius:20px;

    padding:12px;

    box-sizing:border-box;

    box-shadow:
        inset 0 0 10px rgba(255,255,255,0.03), 0 0 10px rgba(0,0,0,0.5);
    
    display:flex;
    flex-direction:column;
    justify-content:space-between;
}

.header {

    display:flex;
    justify-content:space-between;
    align-items:center;

    margin-bottom:10px;
}

.left {

    display:flex;
    align-items:center;
    gap:10px;
}

.status {

    width:15px;
    height:15px;

    border-radius:50%;

    background:red;
}

.label {

    font-size:22px;
    font-weight:bold;
}

.battery {

    width:38px;
    height:16px;

    border:2px solid white;

    border-radius:4px;

    position:relative;
}

.battery::after {

    content:'';

    position:absolute;

    right:-6px;
    top:3px;

    width:4px;
    height:8px;

    background:white;
}

.batteryFill {

    width:0%;
    height:100%;

    background:lime;
}

.zones {

    width:100%;
    height:160px;

    border-radius:18px;

    overflow:hidden;

    display:flex;

    background:#050505;

    box-shadow:
        inset 0 0 10px rgba(255,255,255,0.04),
        inset 0 0 25px rgba(0,0,0,0.7);
}

.zone {

    flex:1;

    height:100%;

    background:black;

    transition:background 0.15s linear;
}

.footer {

    margin-top:10px;

    font-size:18px;
    font-weight:bold;
}

</style>
</head>
<body>

<div class="title">
    DIY TIRE TEMPERATURE DISPLAY
</div>

<div class="container">

<div class="topLeft" id="topLeft"></div>

<div class="topRight" id="topRight"></div>

<div class="legend">

<div class="legendBar"></div>

<div class="legendLabels">
<div>70</div>
<div>100</div>
<div>140</div>
<div>180</div>
<div>220</div>
</div>

</div>

<div class="bottomLeft" id="bottomLeft"></div>

<div class="bottomRight" id="bottomRight"></div>

<script>

const socket =
    new WebSocket(
        `ws://${location.host}/ws`
    );

function createTireHTML(label,index)
{
    return `
    <div class="tire">

        <div class="header">

            <div class="left">
                <div class="status" id="s${index}"></div>
                <div class="label">${label}</div>
            </div>

            <div class="battery">
                <div class="batteryFill" id="b${index}"></div>
            </div>

        </div>

        <div class="zones">

            <div class="zone" id="z${index}_0"></div>
            <div class="zone" id="z${index}_1"></div>
            <div class="zone" id="z${index}_2"></div>
            <div class="zone" id="z${index}_3"></div>
            <div class="zone" id="z${index}_4"></div>

        </div>

        <div class="footer">
            HOT:
            <span id="h${index}">0</span>F
        </div>

    </div>
    `;
}

document.getElementById("topLeft").innerHTML =
    createTireHTML("FL",0);

document.getElementById("topRight").innerHTML =
    createTireHTML("FR",1);

document.getElementById("bottomLeft").innerHTML =
    createTireHTML("RL",2);

document.getElementById("bottomRight").innerHTML =
    createTireHTML("RR",3);

function tempToColor(temp)
{
    temp = Math.max(70, Math.min(220, temp));

    let t =
        (temp - 70)
        /
        (220 - 70);

    let hue =
        240 * (1.0 - t);

    return `hsl(${hue},100%,50%)`;
}

socket.onmessage = (event)=>
{
    const data =
        JSON.parse(event.data);

    for(let i=0;i<4;i++)
    {
        const tire =
            data.tires[i];

        const connected =
            tire.connected;

        document.getElementById(`s${i}`).style.background =
            connected
            ?
            'lime'
            :
            'red';

        for(let z=0;z<5;z++)
        {
            document.getElementById(`z${i}_${z}`).style.background =
                connected
                ?
                tempToColor(tire.zones[z])
                :
                'black';
        }

        document.getElementById(`h${i}`).innerText =
            tire.hot.toFixed(1);

        const fill =
            document.getElementById(`b${i}`);

        fill.style.width =
            `${tire.battery}%`;

        if(tire.battery > 50)
            fill.style.background = 'lime';
        else if(tire.battery > 20)
            fill.style.background = 'yellow';
        else
            fill.style.background = 'red';
    }
};

</script>

</div>
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
// ESP-NOW RECEIVE
// =====================================================

void onDataRecv(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int len) {
  if (len != sizeof(TirePacket))
    return;

  TirePacket pkt;

  memcpy(
    &pkt,
    data,
    sizeof(TirePacket));

  if (pkt.tireID > 3)
    return;

  tirePackets[pkt.tireID] = pkt;

  tireLastSeen[pkt.tireID] = millis();
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  setCpuFrequencyMhz(80);

  Serial.begin(115200);

  pinMode(CAL_PIN, INPUT_PULLUP);
  pinMode(BATTERY_PIN, INPUT);

  calibrationMode =
    (digitalRead(CAL_PIN) == LOW);

  prefs.begin("tirecal", false);

  Wire.begin(MLX_SDA, MLX_SCL);
  Wire.setClock(1000000);

  if (!mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire)) {
    while (true)
      ;
  }

  mlx.setMode(MLX90640_CHESS);
  mlx.setResolution(MLX90640_ADC_18BIT);
  mlx.setRefreshRate(MLX90640_16_HZ);

  WiFi.mode(WIFI_AP_STA);

  if (calibrationMode) {
    WiFi.softAP(calSSID, calPassword);
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


  } else {
    WiFi.softAP(runtimeSSID, runtimePassword);
  }

  ws.onEvent(onWebSocketEvent);

  server.addHandler(&ws);

  server.on(
    "/",
    HTTP_GET,
    [](AsyncWebServerRequest *request) {
      request->send_P(
        200,
        "text/html",
        calibrationMode
          ? calibration_html
          : runtime_html);
    });

  server.begin();

  // =================================================
  // ESP-NOW
  // =================================================

  if (esp_now_init() != ESP_OK) {
    while (true)
      ;
  }

  esp_now_register_recv_cb(onDataRecv);

  Serial.println(WiFi.macAddress());
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  // =================================================
  // READ LOCAL FL THERMAL CAMERA
  // =================================================

  if (mlx.getFrame(frame) == 0) {
    float ambientSum = 0;
    int ambientCount = 0;

    for (int y = 0; y < 24; y++) {
      for (int x = 0; x < 32; x++) {
        if (x < 3 || x > 28 || y < 3 || y > 20) {
          ambientSum +=
            cToF(frame[(y * 32) + x]);

          ambientCount++;
        }
      }
    }

    float ambient = ambientSum / ambientCount;

    float sums[5] = { 0, 0, 0, 0, 0 };
    int counts[5] = { 0, 0, 0, 0, 0 };

    float hottest = ambient;

    float regionWidth =
      (float)(savedMaxX - savedMinX);

    for (int y = savedMinY; y <= savedMaxY; y++) {
      for (int x = savedMinX; x <= savedMaxX; x++) {
        int idx = (y * 32) + x;

        float tempF = cToF(frame[idx]);

        if (tempF > hottest)
          hottest = tempF;

        if (tempF > MAX_ALLOWED_TIRE_TEMP_F)
          continue;

        if (tempF < ambient + 3.0f)
          continue;

        float normalizedX =
          (float)(x - savedMinX)
          / regionWidth;

        int zone = 0;

        if (normalizedX < savedBands[0])
          zone = 0;
        else if (normalizedX < savedBands[1])
          zone = 1;
        else if (normalizedX < savedBands[2])
          zone = 2;
        else if (normalizedX < savedBands[3])
          zone = 3;
        else
          zone = 4;

        sums[zone] += tempF;
        counts[zone]++;
      }
    }

    for (int i = 0; i < 5; i++) {
      float target;

      // =====================================
      // VALID TIRE PIXELS
      // =====================================

      if (counts[i] > 0) {
        target =
          sums[i] / counts[i];
      }

      // =====================================
      // NO HOT PIXELS
      // =====================================

      else {
        // decay back toward ambient

        target =
          ambient;
      }

      // =====================================
      // FIRST FRAME
      // =====================================

      if (firstFrame) {
        smoothedTemps[i] =
          target;
      }

      // =====================================
      // EMA
      // =====================================

      else {
        smoothedTemps[i] =
          (target * 0.08f)
          + (smoothedTemps[i] * 0.92f);
      }

      // =====================================
      // SANITY
      // =====================================

      if (
        isnan(smoothedTemps[i])
        || smoothedTemps[i] < 40
        || smoothedTemps[i] > 250) {
        smoothedTemps[i] =
          ambient;
      }

      tirePackets[0].zone[i] =
        smoothedTemps[i] * 10.0f;
    }

    tirePackets[0].battery =
      readBatteryPercent();

    tirePackets[0].hottest =
      hottest * 10.0f;

    tirePackets[0].tireID = 0;

    tireLastSeen[0] = millis();

    firstFrame = false;
  }

  // =================================================
  // SEND DASHBOARD JSON
  // =================================================

  if (!calibrationMode) {
    String json = "{\"tires\":[";

    for (int i = 0; i < 4; i++) {
      bool connected =
        (millis() - tireLastSeen[i]) < 2000;

      json += "{";

      json += "\"connected\":";
      json += connected ? "true" : "false";

      json += ",\"battery\":";
      json += tirePackets[i].battery;

      json += ",\"hot\":";
      json += String(
        tirePackets[i].hottest / 10.0f,
        1);

      json += ",\"zones\":[";

      for (int z = 0; z < 5; z++) {
        json += String(
          tirePackets[i].zone[z] / 10.0f,
          1);

        if (z < 4)
          json += ",";
      }

      json += "]}";

      if (i < 3)
        json += ",";
    }

    json += "]}";

    ws.textAll(json);

    ws.cleanupClients();
  } else {
    // =============================================
    // GLOBAL TEMPS
    // =============================================

    float minTemp = 999.0f;
    float maxTemp = -999.0f;

    for (int i = 0; i < 768; i++) {
      float t =
        cToF(frame[i]);

      if (t < minTemp)
        minTemp = t;

      if (t > maxTemp)
        maxTemp = t;
    }

    // =============================================
    // THERMAL IMAGE
    // =============================================

    for (int i = 0; i < 768; i++) {
      float tempF =
        cToF(frame[i]);

      float normalized =
        (tempF - minTemp)
        / (maxTemp - minTemp);

      normalized =
        constrain(
          normalized,
          0.0f,
          1.0f);

      frameBytes[i] =
        normalized * 255.0f;
    }

    // =============================================
    // REGION
    // =============================================

    frameBytes[772] = savedMinX;
    frameBytes[773] = savedMaxX;

    frameBytes[774] = savedMinY;
    frameBytes[775] = savedMaxY;

    // =============================================
    // BAND LINES
    // =============================================

    for (int i = 0; i < 4; i++) {
      frameBytes[779 + i] =
        savedBands[i] * 100.0f;
    }

    // =============================================
    // HOT TEMP
    // =============================================

    int16_t hotTemp =
      tirePackets[0].hottest;

    frameBytes[777] =
      hotTemp & 0xFF;

    frameBytes[778] =
      (hotTemp >> 8)
      & 0xFF;

    // =============================================
    // LOW TEMP
    // =============================================

    int16_t lowTemp =
      minTemp * 10.0f;

    frameBytes[784] =
      lowTemp & 0xFF;

    frameBytes[785] =
      (lowTemp >> 8)
      & 0xFF;

    // =============================================
    // BATTERY
    // =============================================

    static int batteryPercent = 100;

    if (
      millis()
        - lastBatteryRead
      > 3000) {
      batteryPercent =
        readBatteryPercent();

      lastBatteryRead =
        millis();
    }

    frameBytes[783] =
      batteryPercent;

    // =============================================
    // STREAM
    // =============================================

    ws.binaryAll(
      frameBytes,
      786);

    ws.cleanupClients();
  }

  delay(5);
}
