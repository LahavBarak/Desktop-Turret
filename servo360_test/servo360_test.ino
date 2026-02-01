#include <SPI.h>

// --- Pin Definitions for ESP32 ---
const int CS_PIN = 5;  // Chip Select pin

// AS5048A Commands
const uint16_t AS5048A_ANGLE = 0x3FFF; // Address for angle register
const uint16_t AS5048A_READ  = 0x4000; // Read command bit
const uint16_t AS5048A_PARITY = 0x8000; // Parity bit placeholder

void setup() {
  Serial.begin(115200);
  
  // Initialize SPI
  SPI.begin(); 
  
  // Configure Chip Select pin
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH); // Deselect the sensor initially

  Serial.println("AS5048A Sensor Ready...");
}

void loop() {
  // 1. Request the angle
  // To read the angle, we send 0xFFFF (Read bit + Address 0x3FFF + Parity)
  // The sensor is "pipelined", meaning the data we read NOW is the result 
  // of the PREVIOUS command.
  uint16_t rawData = readSPI(0xFFFF);

  // 2. Process the data
  // The top bit (15) is parity. Bit 14 is error flag. 
  // The bottom 14 bits (0-13) are the angle value.
  
  // Mask out the top 2 bits to get just the value (0 to 16383)
  uint16_t angleRaw = rawData & 0x3FFF;

  // Check for error bit (Bit 14)
  if (rawData & 0x4000) {
    Serial.print("Error detected! Raw: ");
    Serial.println(rawData, BIN);
  } else {
    // Convert 14-bit value to Degrees (0 to 360)
    float degrees = (float)angleRaw / 16384.0 * 360.0;
    
    Serial.print("Raw: ");
    Serial.print(angleRaw);
    Serial.print("\tDegrees: ");
    Serial.println(degrees, 2);
  }

  delay(100); // Small delay for readability
}

// Helper function to handle SPI transaction
uint16_t readSPI(uint16_t command) {
  // SPI transaction setup
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
  
  // Select the chip
  digitalWrite(CS_PIN, LOW);
  
  // Send command and receive response simultaneously
  uint16_t result = SPI.transfer16(command);
  
  // Deselect the chip
  digitalWrite(CS_PIN, HIGH);
  
  // End transaction
  SPI.endTransaction();
  
  return result;
}