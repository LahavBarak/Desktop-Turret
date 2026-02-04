#include <ESP32Servo.h>
#include <SPI.h>

// --- Pin Definitions ---
const int CS_PIN = D3;   // SPI Chip Select
const int SCK_PIN = D8;  // SPI Clock
const int MISO_PIN = D4; // SPI MISO (Safe Pin)
const int MOSI_PIN = D5; // SPI MOSI (Safe Pin)
const int SERVO_PIN = D2;   // Servo Control Pin

// --- Constants ---
const int STOP_PULSE = 1500;
const int PULSE_START = 2600;
const int PULSE_END = 2600;
const int PULSE_STEP = 200;
const float TARGET_ROTATION = 180.0;
const float NOISE_THRESHOLD = 0.5; // Degrees

Servo myServo;

// Global tracking
float startAngle = 0;
float currentAngle = 0;
float accumulatedAngle = 0;
float lastRawAngle = 0;
bool isTestRunning = false;
bool isSubTestRunning = false;
int currentPulse = PULSE_START;
unsigned long subStartTime = 0;

void setup() {
  // Safety Delay
  delay(1000);
  
  Serial.begin(115200);
  
  // SPI Setup
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
  SPI.setClockDivider(SPI_CLOCK_DIV16); // Global divider for simplicity or use transaction

  // Servo Setup
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  myServo.setPeriodHertz(50); 
  myServo.attach(SERVO_PIN, 500, 3000); // Expanded range for test
  myServo.writeMicroseconds(STOP_PULSE);

  Serial.println("Servo Velocity Test Ready. Send 'g' to Start.");
  
  delay(100);
  float initialAngle;
  readAngleDegrees(initialAngle); // Ignore result for first rough set
  lastRawAngle = initialAngle;
}

void loop() {
  // Check for command
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'g' && !isTestRunning) {
      isTestRunning = true;
      currentPulse = PULSE_START;
      startSubTest();
    }
  }

  if (isTestRunning) {
    if (isSubTestRunning) {
      runSubTest();
    } else {
      // Prepare for next pulse or finish
      currentPulse += PULSE_STEP;
      if (currentPulse > PULSE_END) {
        Serial.println("DONE");
        isTestRunning = false;
        myServo.writeMicroseconds(STOP_PULSE);
      } else {
        // Wait a bit before next run
        delay(1000); 
        startSubTest();
      }
    }
  }
}

void startSubTest() {
  Serial.print("PULSE:");
  Serial.println(currentPulse);
  
  // Reset tracking
  accumulatedAngle = 0;
  float initialAngle;
  // Try up to 5 times to get a valid start angle
  for(int i=0; i<5; i++) {
    if(readAngleDegrees(initialAngle)) break;
    delay(10);
  }
  lastRawAngle = initialAngle;
  startAngle = lastRawAngle; 

  
  // Start Motor
  myServo.writeMicroseconds(currentPulse);
  isSubTestRunning = true;
  subStartTime = millis();
}

void runSubTest() {
  float nowAngle;
  if (!readAngleDegrees(nowAngle)) {
    // reading failed (timeout), skip this frame
    return;
  }
  
  // Calculate delta
  float delta = nowAngle - lastRawAngle;
  
  if (delta < -180) delta += 360;
  else if (delta > 180) delta -= 360;
  
  // Deadband Filter: Ignore noise
  if (abs(delta) > NOISE_THRESHOLD) {
    accumulatedAngle += abs(delta); 
    lastRawAngle = nowAngle;
  }

  // Stream Data: "CALIBRATED_ANGLE,ACCUMULATED,TIME"
  // No need for raw angle in analysis really, but good for debug
  float calibratedAngle = nowAngle - startAngle;
  if (calibratedAngle < 0) calibratedAngle += 360.0;
  
  Serial.print(calibratedAngle, 2);
  Serial.print(",");
  Serial.print(accumulatedAngle, 2);
  Serial.print(",");
  Serial.println(millis() - subStartTime);

  if (millis() - subStartTime >= 1000) {
    myServo.writeMicroseconds(STOP_PULSE);
    isSubTestRunning = false;
    Serial.println("SUB_DONE");
  }
}

// Read Angle from AS5048A via SPI
// Returns true (always valid if wired correctly)
bool readAngleDegrees(float &angle) {
  // SPI Protocol for AS5048A:
  // 1. Send Command (Read Angle = 0xFFFF)
  // 2. Receive Response (previous command result, but for continuous read we pipeline)
  
  // Settings: 1MHz, MSB First, Mode 1 (CPOL=0, CPHA=1)
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
  
  // CS Low to start transaction
  digitalWrite(CS_PIN, LOW);
  
  // Send 0xFFFF (Read Angle) and receive 16-bit response
  uint16_t rawData = SPI.transfer16(0xFFFF);
  
  // CS High to end transaction
  digitalWrite(CS_PIN, HIGH);
  
  SPI.endTransaction();
  
  // Wait a tiny bit? AS5048A needs min 350ns CS high. ESP32 is fast.
  delayMicroseconds(1); 

  // Mask top 2 bits (14 bits resolution)
  // Bit 14 is Error, Bit 15 is Parity. Data is lower 14.
  uint16_t angleData = rawData & 0x3FFF;
  
  angle = (float)angleData / 16383.0 * 360.0;
  return true;
}
