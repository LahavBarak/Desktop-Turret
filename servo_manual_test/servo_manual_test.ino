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
const int PWM_SLOW = 2400; // Slowly Move one way

Servo myServo;

// Global tracking
float currentAngle = 0;
int currentPWM = PWM_STOP;

// Function Prototypes
float readAngleSPI();

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

  // Wait for Serial
  unsigned long startWait = millis();
  while (!Serial && millis() - startWait < 5000) {
    delay(10);
  }
  Serial.println("Manual Control Ready.");
  Serial.println("Commands:");
  Serial.println(" g : Go Slow (PWM 1550)");
  Serial.println(" s : Stop (PWM 1500)");
}

void loop() {
  // 1. Handle Serial Input
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'g') {
      currentPWM = PWM_SLOW;
      Serial.println("CMD: GO");
    } else if (c == 's') {
      currentPWM = PWM_STOP;
      Serial.println("CMD: STOP");
    }
    
    // Update Servo
    myServo.writeMicroseconds(currentPWM);
  }

  // 2. Read and Stream Data
  currentAngle = readAngleSPI();
  
  Serial.print("Angle:");
  Serial.print(currentAngle);
  Serial.print(" PWM:");
  Serial.println(currentPWM);
  
  // 3. Heartbeat
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 500) {
    lastBlink = millis();
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  
  delay(50); // Simple loop delay
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
