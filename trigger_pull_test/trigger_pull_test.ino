#include <ESP32Servo.h>

// --- Pin Definitions ---
const int ENCODER_PIN = D1; // PWM Input from AS5048A
const int SERVO_PIN = D2;   // Servo Control Pin

// --- Constants ---
const int STOP_PULSE = 1500;
const int ROTATE_PULSE = 2500; 
const float TARGET_ROTATION = 180.0;

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
  
  // Encoder Setup
  pinMode(ENCODER_PIN, INPUT);

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

// Read Angle from AS5048A via PWM
float readAngleDegrees() {
  // AS5048A PWM Frequency ~1kHz (1024us period usually)
  // Timeout 2500us to be safe (> 1 cycle)
  unsigned long highTime = pulseIn(ENCODER_PIN, HIGH, 3000); 
  unsigned long lowTime  = pulseIn(ENCODER_PIN, LOW, 3000);
  unsigned long totalTime = highTime + lowTime;

  if (totalTime == 0) return lastRawAngle; // Timeout or Error, hold last value

  float dutyCycle = (float)highTime / (float)totalTime;
  // AS5048A clamps between ~3% and 97% effectively. 
  // Should we map? For simple relative test, raw duty mapping is OK.
  // Standard formula:
  float degrees = dutyCycle * 360.0;
  
  return degrees;
}
