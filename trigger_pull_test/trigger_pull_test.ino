#include <ESP32Servo.h>
#include <SPI.h>

// --- Pin Definitions ---
const int CS_PIN = D3;   // SPI Chip Select
const int SCK_PIN = D8;  // SPI Clock
const int MISO_PIN = D4; // SPI MISO (Changed from D9 to avoid Strapping Pin)
const int MOSI_PIN = D5; // SPI MOSI (Changed from D10 via Plan)

const int SERVO_PIN = D2;   // Servo Control Pin

// --- Constants ---
const int STOP_PULSE = 1500;
const int ROTATE_PULSE = 2500; 
const float TARGET_ROTATION = 180.0;
const float NOISE_THRESHOLD = 0.5; // Degrees

Servo myServo;

// Global tracking
float startAngle = 0;
float currentAngle = 0;
float accumulatedAngle = 0;
float lastRawAngle = 0;
bool isRotating = false;
bool loggingEnabled = true;

void setup() {
  Serial.begin(115200);
  
  // Wait for Serial (up to 5s)
  unsigned long startWait = millis();
  while (!Serial && millis() - startWait < 5000) {
    delay(10);
  }
  Serial.println("Booting...");

  // Xiao ESP32S3 User LED is GPIO 21 (Yellow)
  pinMode(21, OUTPUT);
  
  // SPI Setup
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH); // Deselect
  
  // Initialize SPI with custom pins
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
  
  // AS5048A Max SPI speed 10MHz. Safe 1MHz.
  SPI.setClockDivider(SPI_CLOCK_DIV16); // or SPI.beginTransaction() settings

  // Servo Setup
  // Allow allocation of all timers
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  myServo.setPeriodHertz(50); 
  myServo.attach(SERVO_PIN, 500, 2400);
  myServo.writeMicroseconds(STOP_PULSE);

  Serial.println("Trigger Pull Test Ready (PWM Mode). Send 'g' for Data, 's' for Silent.");
  
  // Initial read to set lastRawAngle
  delay(100);
  lastRawAngle = readAngleDegrees();
}

void loop() {
  // Check for command
  if (Serial.available()) {
    char c = Serial.read();
    if ((c == 'g' || c == 's') && !isRotating) {
      loggingEnabled = (c == 'g');
      if (loggingEnabled) Serial.println("STARTING");
      else Serial.println("FIRING");
      
      // Reset tracking
      accumulatedAngle = 0;
      lastRawAngle = readAngleDegrees();
      startAngle = lastRawAngle; 
      
      // Start Motor
      myServo.writeMicroseconds(ROTATE_PULSE);
      isRotating = true;
    }
  }

  if (isRotating) {
    float nowAngle = readAngleDegrees();
    
    // Ignore invalid readings (0 or near 0 very often means disconnected/error in PWM)
    // But 0 is valid angle. AS5048A PWM usually has 12-bit res.
    // If pulseIn times out, it returns 0. totalTime will be 0.
    // Let's rely on readAngleDegrees handling validation or just accept noise for now.
    
    // Calculate delta and handle wrap-around
    float delta = nowAngle - lastRawAngle;
    
    if (delta < -180) delta += 360;
    else if (delta > 180) delta -= 360;
    
    accumulatedAngle += abs(delta); 
    lastRawAngle = nowAngle;

    // Stream Data: "CALIBRATED_ANGLE,ACCUMULATED,RAW_ANGLE"
    if (loggingEnabled) {
      // Calibrate Angle (Relative to Start)
      float calibratedAngle = nowAngle - startAngle;
      if (calibratedAngle < 0) calibratedAngle += 360.0;
      
      Serial.print(calibratedAngle, 2);
      Serial.print(",");
      Serial.println(accumulatedAngle, 2);
    }

    if (accumulatedAngle >= TARGET_ROTATION) {
      myServo.writeMicroseconds(STOP_PULSE);
      isRotating = false;
      if (loggingEnabled) Serial.println("DONE");
      else Serial.println("FINISHED");
    }
  }
}

// Read Angle from AS5048A via SPI
// Returns true (always valid if wired correctly, unlike PWM timeout)
float readAngleDegrees() {
  // SPI Protocol for AS5048A:
  // 1. Send Command (Read Angle = 0xFFFF)
  // 2. Receive Response (previous command result, but for continuous read we pipeline)
  
  // CS Low to start transaction
  digitalWrite(CS_PIN, LOW);
  
  // Send 0xFFFF (Read Angle) and receive 16-bit response
  // AS5048A expects 16 bits. SPI.transfer16() handles this.
  uint16_t rawData = SPI.transfer16(0xFFFF);
  
  // CS High to end transaction
  digitalWrite(CS_PIN, HIGH);
  
  // Wait a tiny bit? AS5048A needs min 350ns CS high. ESP32 is fast.
  delayMicroseconds(1); 
  
  // Mask top 2 bits (14 bits resolution)
  // Bit 14 is Error, Bit 15 is Parity. Data is lower 14.
  uint16_t angleData = rawData & 0x3FFF;
  
  float degrees = (float)angleData / 16383.0 * 360.0;
  return degrees;
}
