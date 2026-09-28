/*
 * IMU BLE Logger - firmware
 * -------------------------
 * Records 6-axis inertial data (accelerometer + gyroscope + temperature) at a
 * fixed 100 Hz on a Seeed XIAO nRF52840 Sense, stores it in RAM in a compact
 * binary format, and exposes everything over Bluetooth Low Energy (BLE):
 *
 *   - a decimated live stream (10 Hz) for on-screen display,
 *   - a command channel (calibrate / start / stop / download / refresh),
 *   - a reliable bulk download of the full-rate recording.
 *
 * The recording is the reference data. It survives phone disconnections and
 * page reloads, but is lost if the board is powered off or reset.
 *
 * Why RAM and not the internal flash?
 *   The internal LittleFS partition of the nRF52840 is only ~28 KB (about 3 s
 *   of CSV text at 100 Hz), and flash erase cycles stall the sampling loop.
 *   Storing 18-byte binary records in RAM gives 60 s at a perfectly regular
 *   100 Hz. See README.md ("Design decisions") for the measurements.
 *
 * Protocol documentation: docs/PROTOCOL.md
 */

#include <Arduino.h>
#include <bluefruit.h>
#include "LSM6DS3.h"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#define DEVICE_NAME    "IMU-LOGGER"   // BLE name; the web dashboard filters on it
#define MAX_SAMPLES    6000           // 60 s at 100 Hz = 108 KB of RAM
#define LIVE_DIVIDER   10             // send 1 live packet out of N samples (10 Hz)
#define FILE_CHUNK_MAX 180            // max bytes per download notification
#define WANTED_MTU     247            // ATT MTU requested from the phone

static const uint32_t SAMPLE_PERIOD_US = 10000;  // 100 Hz
static const uint16_t CALIBRATION_SAMPLES = 500; // 5 s at 100 Hz

// ---------------------------------------------------------------------------
// BLE UUIDs (custom 128-bit service)
// ---------------------------------------------------------------------------
#define SERVICE_UUID "7c6e0001-6f3d-4f9d-9b5e-8f4f2a1b1000"
#define LIVE_UUID    "7c6e0002-6f3d-4f9d-9b5e-8f4f2a1b1000"
#define CMD_UUID     "7c6e0003-6f3d-4f9d-9b5e-8f4f2a1b1000"
#define STATUS_UUID  "7c6e0004-6f3d-4f9d-9b5e-8f4f2a1b1000"
#define OFFSET_UUID  "7c6e0005-6f3d-4f9d-9b5e-8f4f2a1b1000"
#define DATA_UUID    "7c6e0006-6f3d-4f9d-9b5e-8f4f2a1b1000"
#define META_UUID    "7c6e0007-6f3d-4f9d-9b5e-8f4f2a1b1000"

// ---------------------------------------------------------------------------
// Data structures (all little-endian, packed)
// ---------------------------------------------------------------------------

// Stored sample: 18 bytes.
//   accel scaled x8000 (+-4 g range), gyro scaled x64 (+-500 dps range),
//   temperature scaled x100.
struct __attribute__((packed)) Record {
  uint32_t t_us;
  int16_t ax, ay, az, gx, gy, gz, temp;
};

// Header sent at the start of a download: 20 bytes.
struct __attribute__((packed)) Header {
  uint32_t magic;                   // HEADER_MAGIC
  uint32_t count;                   // number of records that follow
  float offsetX, offsetY, offsetZ;  // gyro offsets (deg/s) from the last calibration
};
#define HEADER_MAGIC 0x31554D49UL   // "IMU1" (format version 1)

// Live packet: 28 bytes, sent at 10 Hz while recording.
struct __attribute__((packed)) LivePacket {
  uint32_t seq, t_us;
  int16_t ax, ay, az;              // g x1000
  int16_t gxRaw, gyRaw, gzRaw;     // deg/s x10
  int16_t gxCorr, gyCorr, gzCorr;  // deg/s x10, offset removed
  int16_t temp;                    // deg C x100
};

// Calibration result: 24 bytes.
struct __attribute__((packed)) OffsetPacket {
  float x, y, z;      // mean gyro reading at rest (deg/s)
  float sx, sy, sz;   // standard deviation (deg/s)
};

// Recording metadata: 12 bytes.
struct __attribute__((packed)) MetaPacket {
  uint32_t samples;   // number of stored samples
  uint32_t bytes;     // size of the download stream (header + records)
  uint8_t valid;      // 1 if a recording is available
  uint8_t pad[3];
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
LSM6DS3 imu(I2C_MODE, 0x6A);

BLEService serviceSvc(SERVICE_UUID);
BLECharacteristic liveChr(LIVE_UUID), cmdChr(CMD_UUID), statusChr(STATUS_UUID),
                  offsetChr(OFFSET_UUID), dataChr(DATA_UUID), metaChr(META_UUID);

static Record buffer[MAX_SAMPLES];

bool imuOk = false, recording = false, calibrating = false;
uint32_t nextSampleUs = 0, nextCalUs = 0, seq = 0, count = 0;
float offX = 0, offY = 0, offZ = 0, sdX = 0, sdY = 0, sdZ = 0;
uint16_t calCount = 0;
double sumX = 0, sumY = 0, sumZ = 0, sumX2 = 0, sumY2 = 0, sumZ2 = 0;
uint16_t connHandle = BLE_CONN_HANDLE_INVALID;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Round and saturate a float to int16 after scaling.
static inline int16_t quantize(float value, float scale) {
  float r = lroundf(value * scale);
  if (r > 32767) r = 32767;
  if (r < -32768) r = -32768;
  return (int16_t)r;
}

static uint32_t streamBytes() {
  return count ? (sizeof(Header) + count * sizeof(Record)) : 0;
}

// Send a BLE notification, retrying while the stack's queue is full.
// A plain notify() returns false when the queue is full and the data is lost,
// so every notification goes through this helper.
bool notifyReliable(BLECharacteristic &chr, const void *data, uint16_t len, uint8_t maxTries = 20) {
  if (!chr.notifyEnabled()) return false;
  for (uint8_t i = 0; i < maxTries; i++) {
    if (chr.notify((void *)data, len)) return true;
    if (!Bluefruit.connected()) return false;
    delay(5);
  }
  return false;
}

void sendStatus(const char *s) {
  notifyReliable(statusChr, s, (uint16_t)min((size_t)strlen(s), (size_t)20));
}

void sendMeta() {
  MetaPacket m{count, streamBytes(), (uint8_t)(count > 0), {0, 0, 0}};
  notifyReliable(metaChr, &m, sizeof(m));
}

void sendOffsets() {
  OffsetPacket p{offX, offY, offZ, sdX, sdY, sdZ};
  notifyReliable(offsetChr, &p, sizeof(p));
}

void sendCurrentStatus() {
  sendStatus(recording ? "RECORDING" : (calibrating ? "CALIBRATING" : (imuOk ? "READY" : "IMU_ERROR")));
}

void readImu(float &ax, float &ay, float &az, float &gx, float &gy, float &gz, float &tempC) {
  ax = imu.readFloatAccelX(); ay = imu.readFloatAccelY(); az = imu.readFloatAccelZ();
  gx = imu.readFloatGyroX();  gy = imu.readFloatGyroY();  gz = imu.readFloatGyroZ();
  tempC = imu.readTempC();
}

// ---------------------------------------------------------------------------
// Calibration: average the gyro at rest for 5 s to estimate its offset
// ---------------------------------------------------------------------------
void beginCalibration() {
  if (!imuOk || recording) return;
  calibrating = true;
  calCount = 0;
  sumX = sumY = sumZ = sumX2 = sumY2 = sumZ2 = 0;
  nextCalUs = micros();
  sendStatus("CALIBRATING");
}

void endCalibration() {
  offX = sumX / CALIBRATION_SAMPLES;
  offY = sumY / CALIBRATION_SAMPLES;
  offZ = sumZ / CALIBRATION_SAMPLES;
  sdX = sqrt(max(0.0, sumX2 / CALIBRATION_SAMPLES - offX * offX));
  sdY = sqrt(max(0.0, sumY2 / CALIBRATION_SAMPLES - offY * offY));
  sdZ = sqrt(max(0.0, sumZ2 / CALIBRATION_SAMPLES - offZ * offZ));
  calibrating = false;
  sendOffsets();
  sendStatus("CALIBRATED");
}

void calibrationLoop() {
  uint32_t now = micros();
  if ((int32_t)(now - nextCalUs) < 0) return;
  nextCalUs += SAMPLE_PERIOD_US;
  float ax, ay, az, gx, gy, gz, t;
  readImu(ax, ay, az, gx, gy, gz, t);
  sumX += gx; sumY += gy; sumZ += gz;
  sumX2 += gx * gx; sumY2 += gy * gy; sumZ2 += gz * gz;
  if (++calCount >= CALIBRATION_SAMPLES) endCalibration();
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------
void beginRecording() {
  if (!imuOk || calibrating) return;
  count = 0;
  seq = 0;
  recording = true;
  nextSampleUs = micros();
  sendStatus("RECORDING");
}

void endRecording() {
  recording = false;
  Serial.print("[REC] done, samples=");
  Serial.println(count);
  sendMeta();
  sendStatus("SAVED");
}

void takeSample() {
  float ax, ay, az, gx, gy, gz, tempC;
  readImu(ax, ay, az, gx, gy, gz, tempC);
  uint32_t t = micros(), s = seq++;

  buffer[count++] = Record{t, quantize(ax, 8000), quantize(ay, 8000), quantize(az, 8000),
                           quantize(gx, 64), quantize(gy, 64), quantize(gz, 64),
                           quantize(tempC, 100)};

  if (s % LIVE_DIVIDER == 0 && liveChr.notifyEnabled()) {
    LivePacket p{s, t,
                 quantize(ax, 1000), quantize(ay, 1000), quantize(az, 1000),
                 quantize(gx, 10), quantize(gy, 10), quantize(gz, 10),
                 quantize(gx - offX, 10), quantize(gy - offY, 10), quantize(gz - offZ, 10),
                 quantize(tempC, 100)};
    liveChr.notify((uint8_t *)&p, sizeof(p));
  }

  if (count >= MAX_SAMPLES) {  // buffer full: stop and keep the recording
    endRecording();
    sendStatus("FULL");
  }
}

void recordingLoop() {
  uint32_t now = micros();
  if ((int32_t)(now - nextSampleUs) < 0) return;
  nextSampleUs += SAMPLE_PERIOD_US;
  // If we fell far behind, resynchronize instead of bursting to catch up.
  if ((int32_t)(now - nextSampleUs) > (int32_t)(5 * SAMPLE_PERIOD_US)) nextSampleUs = now + SAMPLE_PERIOD_US;
  takeSample();
}

// ---------------------------------------------------------------------------
// Download: header, then the raw records, in chunks sized to the negotiated MTU
// ---------------------------------------------------------------------------
void transferData() {
  if (count == 0) { sendStatus("NO_DATA"); return; }
  if (recording || calibrating) { sendStatus("BUSY"); return; }

  uint32_t total = streamBytes();
  sendMeta();  // guarantees the browser knows the exact expected size
  sendStatus("DOWNLOAD");

  uint16_t chunkLen = 20;  // safe fallback if the MTU exchange did not happen
  if (connHandle != BLE_CONN_HANDLE_INVALID) {
    BLEConnection *c = Bluefruit.Connection(connHandle);
    if (c) {
      uint16_t mtu = c->getMtu();
      if (mtu > 3) chunkLen = mtu - 3;
    }
  }
  if (chunkLen > FILE_CHUNK_MAX) chunkLen = FILE_CHUNK_MAX;
  Serial.print("[DL] start, bytes=");
  Serial.print(total);
  Serial.print(" chunkLen=");
  Serial.println(chunkLen);

  Header h{HEADER_MAGIC, count, offX, offY, offZ};
  uint32_t sent = 0;
  if (!notifyReliable(dataChr, &h, sizeof(h), 100)) { sendStatus("TRANSFER_ERROR"); return; }
  sent += sizeof(h);

  const uint8_t *p = (const uint8_t *)buffer;
  uint32_t dataLen = count * sizeof(Record), offset = 0;
  while (offset < dataLen) {
    if (!Bluefruit.connected()) {
      Serial.println("[DL] aborted: disconnected");
      return;
    }
    uint16_t n = (dataLen - offset > chunkLen) ? chunkLen : (uint16_t)(dataLen - offset);
    if (!notifyReliable(dataChr, p + offset, n, 100)) {
      Serial.print("[DL] aborted: notify failed at byte ");
      Serial.println(sent);
      sendStatus("TRANSFER_ERROR");
      return;
    }
    offset += n;
    sent += n;
    delay(2);
  }
  Serial.print("[DL] done, sent=");
  Serial.print(sent);
  Serial.print("/");
  Serial.println(total);
  sendStatus("DOWNLOAD_DONE");
}

// ---------------------------------------------------------------------------
// BLE callbacks
// ---------------------------------------------------------------------------
void onCommand(uint16_t, BLECharacteristic *, uint8_t *data, uint16_t len) {
  if (!len) return;
  switch (data[0]) {
    case 'C': beginCalibration(); break;
    case 'S': beginRecording(); break;
    case 'E': if (recording) endRecording(); break;
    case 'D': transferData(); break;
    case 'M': sendMeta(); break;
  }
}

// Push the current state as soon as a client subscribes, so a (re)connecting
// dashboard is immediately in sync (e.g. reconnecting during a recording).
void onStatusSubscribed(uint16_t, BLECharacteristic *, uint16_t) { sendCurrentStatus(); }
void onMetaSubscribed(uint16_t, BLECharacteristic *, uint16_t)   { sendMeta(); }
void onOffsetSubscribed(uint16_t, BLECharacteristic *, uint16_t) { sendOffsets(); }

void onConnect(uint16_t conn_handle) {
  connHandle = conn_handle;
  // A new connection starts at ATT MTU 23 (20 usable bytes), which is too
  // small for our packets. Ask for a larger MTU and data length.
  BLEConnection *c = Bluefruit.Connection(conn_handle);
  if (c) {
    c->requestMtuExchange(WANTED_MTU);
    c->requestDataLengthUpdate();
  }
}

void onDisconnect(uint16_t, uint8_t) { connHandle = BLE_CONN_HANDLE_INVALID; }

void setupBle() {
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);  // must be called before begin()
  Bluefruit.begin(1, 0);
  Bluefruit.setTxPower(4);
  Bluefruit.setName(DEVICE_NAME);
  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);

  serviceSvc.begin();

  liveChr.setProperties(CHR_PROPS_NOTIFY);
  liveChr.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  liveChr.setFixedLen(sizeof(LivePacket));
  liveChr.begin();

  cmdChr.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
  cmdChr.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);
  cmdChr.setFixedLen(1);
  cmdChr.setWriteCallback(onCommand);
  cmdChr.begin();

  statusChr.setProperties(CHR_PROPS_NOTIFY);
  statusChr.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  statusChr.setMaxLen(20);
  statusChr.setCccdWriteCallback(onStatusSubscribed);
  statusChr.begin();

  offsetChr.setProperties(CHR_PROPS_NOTIFY);
  offsetChr.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  offsetChr.setFixedLen(sizeof(OffsetPacket));
  offsetChr.setCccdWriteCallback(onOffsetSubscribed);
  offsetChr.begin();

  dataChr.setProperties(CHR_PROPS_NOTIFY);
  dataChr.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  dataChr.setMaxLen(FILE_CHUNK_MAX);
  dataChr.begin();

  metaChr.setProperties(CHR_PROPS_NOTIFY);
  metaChr.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  metaChr.setFixedLen(sizeof(MetaPacket));
  metaChr.setCccdWriteCallback(onMetaSubscribed);
  metaChr.begin();

  Bluefruit.Periph.setConnInterval(6, 12);  // 7.5-15 ms, best effort (the phone decides)
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(serviceSvc);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.start(0);
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  imu.settings.accelRange = 4;         // +-4 g
  imu.settings.gyroRange = 500;        // +-500 deg/s
  imu.settings.accelSampleRate = 104;  // Hz (closest supported rate to 100)
  imu.settings.gyroSampleRate = 104;
  imuOk = (imu.begin() == 0);
  setupBle();
}

void loop() {
  if (calibrating) { calibrationLoop(); return; }
  if (recording) recordingLoop();
}
