#include <ESP32Servo.h>
#include <SPI.h>

// --- Pin Definitions ---
const int CS_PIN = D3;   // SPI Chip Select
const int SCK_PIN = D8;  // SPI Clock
const int MISO_PIN = D4; // SPI MISO
const int MOSI_PIN = D5; // SPI MOSI
const int SERVO_PIN = D2;

// --- Servo ---
const int STOP_PULSE   = 1500;
const int ROTATE_PULSE = 2500;

// --- State machine thresholds ---
const float CRUISE_THRESHOLD = 2.0;  // delta/step to be considered cruising
const int   CRUISE_CONFIRM   = 3;    // consecutive steps above threshold to enter CRUISE
const float LOAD_THRESHOLD   = 1.5;  // delta/step below this = spring engaging
const float FIRE_THRESHOLD   = 2.0;  // delta/step above this after loading = trigger fired
const float SAFETY_ROTATION  = 360.0; // fallback stop if state machine misses

enum State { ACCEL, CRUISE, LOADING, FIRED };

Servo myServo;

float startAngle       = 0;
float accumulatedAngle = 0;
float lastRawAngle     = 0;
bool  isRotating       = false;
bool  loggingEnabled   = true;
State state            = ACCEL;
int   cruiseCount      = 0;

void setup() {
  Serial.begin(115200);
  unsigned long startWait = millis();
  while (!Serial && millis() - startWait < 5000) delay(10);
  Serial.println("Booting...");

  pinMode(21, OUTPUT);

  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);
  myServo.writeMicroseconds(STOP_PULSE);

  delay(100);
  clearErrors();
  lastRawAngle = readAngleDegrees();

  Serial.println("Ready. Send 'g' (log) or 's' (silent).");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if ((c == 'g' || c == 's') && !isRotating) {
      loggingEnabled = (c == 'g');
      Serial.println(loggingEnabled ? "STARTING" : "FIRING");

      accumulatedAngle = 0;
      lastRawAngle     = readAngleDegrees();
      startAngle       = lastRawAngle;
      state            = ACCEL;
      cruiseCount      = 0;

      myServo.writeMicroseconds(ROTATE_PULSE);
      isRotating = true;
    }
  }

  if (isRotating) {
    float nowAngle = readAngleDegrees();
    float delta    = nowAngle - lastRawAngle;
    if (delta < -180) delta += 360;
    else if (delta > 180) delta -= 360;
    delta = abs(delta);

    accumulatedAngle += delta;
    lastRawAngle      = nowAngle;

    if (loggingEnabled) {
      float calibrated = nowAngle - startAngle;
      if (calibrated < 0) calibrated += 360.0;
      Serial.print(calibrated, 2);
      Serial.print(",");
      Serial.print(accumulatedAngle, 2);
      Serial.print(",");
      Serial.println(delta, 3);
    }

    // State machine
    switch (state) {
      case ACCEL:
        if (delta >= CRUISE_THRESHOLD) {
          if (++cruiseCount >= CRUISE_CONFIRM) state = CRUISE;
        } else {
          cruiseCount = 0;
        }
        break;

      case CRUISE:
        if (delta < LOAD_THRESHOLD) {
          state = LOADING;
          if (loggingEnabled) Serial.println("SPRING_ENGAGED");
        }
        break;

      case LOADING:
        if (delta >= FIRE_THRESHOLD) {
          state = FIRED;
        }
        break;

      case FIRED:
        break;
    }

    // Stop on trigger fire or safety limit
    if (state == FIRED || accumulatedAngle >= SAFETY_ROTATION) {
      myServo.writeMicroseconds(STOP_PULSE);
      isRotating = false;

      if (state == FIRED && loggingEnabled) Serial.println("TRIGGERED");
      if (accumulatedAngle >= SAFETY_ROTATION) Serial.println("SAFETY_STOP");

      // Measure coast
      float angleAtStop = accumulatedAngle;
      float coastAngle  = accumulatedAngle;
      float coastLast   = lastRawAngle;
      unsigned long t   = millis();
      while (millis() - t < 1000) {
        float a = readAngleDegrees();
        float d = a - coastLast;
        if (d < -180) d += 360;
        else if (d > 180) d -= 360;
        coastAngle += abs(d);
        coastLast = a;
        delay(10);
      }
      Serial.print("COAST=");
      Serial.println(coastAngle - angleAtStop, 2);
      Serial.println("DONE");
    } else {
      delay(10);
    }
  }
}

// Read and discard the AS5048A error register to clear the latching error flag
void clearErrors() {
  static const SPISettings settings(1000000, MSBFIRST, SPI_MODE1);
  static const uint16_t CMD_READ_ERROR = 0x4001;

  SPI.beginTransaction(settings);
  digitalWrite(CS_PIN, LOW);
  SPI.transfer16(CMD_READ_ERROR);
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
  delayMicroseconds(1);
  SPI.beginTransaction(settings);
  digitalWrite(CS_PIN, LOW);
  SPI.transfer16(0x0000);
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
}

// Read Angle from AS5048A via SPI (pipelined)
float readAngleDegrees() {
  static const SPISettings settings(1000000, MSBFIRST, SPI_MODE1);
  static const uint16_t CMD_READ_ANGLE = 0xFFFF;

  SPI.beginTransaction(settings);
  digitalWrite(CS_PIN, LOW);
  SPI.transfer16(CMD_READ_ANGLE);
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
  delayMicroseconds(1);
  SPI.beginTransaction(settings);
  digitalWrite(CS_PIN, LOW);
  uint16_t rawData = SPI.transfer16(0x0000);
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();

  return (float)(rawData & 0x3FFF) / 16383.0 * 360.0;
}
