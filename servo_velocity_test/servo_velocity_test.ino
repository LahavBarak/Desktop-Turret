#include <ESP32Servo.h>

// --- Pin Definitions ---
const int ENCODER_PIN = D1; // PWM Input from AS5048A
const int SERVO_PIN = D2;   // Servo Control Pin

// --- Constants ---
const int STOP_PULSE = 1500;
const int PULSE_START = 1600;
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
  Serial.begin(115200);
  
  // Encoder Setup
  pinMode(ENCODER_PIN, INPUT);

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

// Read Angle from AS5048A via PWM
// Returns true if valid reading, false if timeout
bool readAngleDegrees(float &angle) {
  unsigned long highTime = pulseIn(ENCODER_PIN, HIGH, 3000); 
  unsigned long lowTime  = pulseIn(ENCODER_PIN, LOW, 3000);
  unsigned long totalTime = highTime + lowTime;

  if (totalTime == 0) return false; 

  float dutyCycle = (float)highTime / (float)totalTime;
  angle = dutyCycle * 360.0;
  
  return true;
}
