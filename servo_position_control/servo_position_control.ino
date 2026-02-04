#include <ESP32Servo.h>
#include <SPI.h>

// --- Pin Definitions (SAFE PINOUT) ---
const int CS_PIN = D3;   // SPI Chip Select
const int SCK_PIN = D8;  // SPI Clock
const int MISO_PIN = D4; // SPI MISO (Safe Pin)
const int MOSI_PIN = D5; // SPI MOSI (Safe Pin)
const int SERVO_PIN = D2;   // Servo Control Pin
const int LED_PIN = 21;     // Xiao ESP32S3 User LED

// --- Constants ---
const int PWM_STOP = 1500;
const int PWM_MIN = 1000;
const int PWM_MAX = 2000;

// P-Controller Gains
// Kp: How hard to push per degree of error. 
// E.g., Error=10 deg, Kp=5 -> Speed=50. Pulse=1550.
float Kp = 6.0; 
float DEAD_ZONE = 1.0; // Degrees. Stop if within this range.

Servo myServo;

// Global tracking
float targetAngle = 0; // Target in degrees (0-360)
float currentAngle = 0;
bool isEnabled = false;

// Function Prototypes
float readAngleSPI();
void runControlLoop();

void setup() {
  // Safety Delay for USB
  delay(3000);
  
  Serial.begin(115200);
  
  // LED Setup
  pinMode(LED_PIN, OUTPUT);
  
  // SPI Setup
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
  
  // Servo Setup
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  myServo.setPeriodHertz(50); 
  myServo.attach(SERVO_PIN, 500, 3000);
  myServo.writeMicroseconds(PWM_STOP);

  // Wait for Serial (Optional but good for debug)
  unsigned long startWait = millis();
  while (!Serial && millis() - startWait < 5000) {
    delay(10);
  }
  Serial.println("Position Control Ready.");
  Serial.println("Send angle (0-360) to move.");
  
  // Initial current angle
  currentAngle = readAngleSPI();
  targetAngle = currentAngle; // Start where we are
}

void loop() {
  // 1. Handle Serial Input
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) {
      if (input.startsWith("K:")) {
        // Update Kp? "K:5.5"
        Kp = input.substring(2).toFloat();
        Serial.print("Kp set to: "); Serial.println(Kp);
      } else {
        // Assume pure number as target
        float newTarget = input.toFloat();
        // Normalize to 0-360
        while(newTarget >= 360) newTarget -= 360;
        while(newTarget < 0) newTarget += 360;
        
        targetAngle = newTarget;
        isEnabled = true;
        Serial.print("Target: "); Serial.println(targetAngle);
      }
    }
  }

  // 2. Run Control Loop at ~50Hz (20ms)
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate >= 20) {
    lastUpdate = millis();
    if (isEnabled) {
      runControlLoop();
    }
  }
  
  // 3. Heartbeat
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 500) {
    lastBlink = millis();
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
}

void runControlLoop() {
  currentAngle = readAngleSPI();
  
  // Calculate Shortest Path Error
  float error = targetAngle - currentAngle;
  
  if (error > 180) error -= 360;
  if (error < -180) error += 360;
  
  // Stream Data for Plotter
  // Format: "Target,Current"
  Serial.print(targetAngle);
  Serial.print(",");
  Serial.println(currentAngle);
  
  int pwmSignal = PWM_STOP;
  
  if (abs(error) > DEAD_ZONE) {
    // P-Control
    float controlEffort = error * Kp;
    
    // Clamp Speed (Prevent going too crazy?) 
    // Usually servo hardware limit handles raw 1000-2000 clamping.
    
    // Calculate PWM
    // CHANGE DIRECTION: If motor runs away, change '+' to '-' here.
    int dir = 1; 
    
    // Stiction Compensation (Minimum Speed)
    // Continuous Rotation servos have a "deadband" around 1500 where they don't move.
    // We need to jump over this.
    int minSpeed = 40; 
    
    // Apply Min Speed
    if (abs(controlEffort) < minSpeed && abs(controlEffort) > 0.1) {
      if (controlEffort > 0) controlEffort = minSpeed;
      else controlEffort = -minSpeed;
    }

    pwmSignal = PWM_STOP + (int)(dir * controlEffort);
     
    // Clamp to hardware limits
    if (pwmSignal > 2000) pwmSignal = 2000;
    if (pwmSignal < 1000) pwmSignal = 1000;
    
  } else {
    // In dead zone, stop
    pwmSignal = PWM_STOP;
  }
  
  // Debug PWM
  // Serial.print(" PWM:"); Serial.println(pwmSignal);
  
  myServo.writeMicroseconds(pwmSignal);
}

// Read Angle from AS5048A via SPI (Mode 1)
float readAngleSPI() {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
  digitalWrite(CS_PIN, LOW);
  uint16_t rawData = SPI.transfer16(0xFFFF);
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
  
  // Tiny delay
  delayMicroseconds(1);
  
  uint16_t angleData = rawData & 0x3FFF;
  return (float)angleData / 16383.0 * 360.0;
}
