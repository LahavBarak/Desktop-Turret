// --- AS5048A PWM ANGLE READER ---
// A simple alternative to SPI to verify the sensor is alive.

const int PWM_PIN = D1; // Connect Sensor PWM to XIAO D1 (GPIO 2)

void setup() {
  Serial.begin(115200);
  while (!Serial);

  pinMode(PWM_PIN, INPUT); // Set as input to read the pulse
  
  Serial.println("--- AS5048A PWM TEST ---");
  Serial.println("Reading pulse width...");
}

void loop() {
  // 1. Measure the length of the HIGH pulse (in microseconds)
  // timeout is 20000us (20ms) to prevent hanging if disconnected
  unsigned long highTime = pulseIn(PWM_PIN, HIGH, 20000); 
  
  // 2. Measure the length of the LOW pulse
  unsigned long lowTime  = pulseIn(PWM_PIN, LOW, 20000);

  // 3. Calculate Total Cycle Time (Period)
  unsigned long totalTime = highTime + lowTime;

  // 4. Validate Data
  if (totalTime == 0) {
    // If totalTime is 0, we aren't seeing any pulses (Wire broken or Sensor Dead)
    Serial.println("Error: No Signal Detected (Check Wire / Power)");
  } 
  else {
    // 5. Calculate Angle
    // The angle is the ratio of "On Time" to "Total Time"
    // Formula: Angle = 360 * (HighTime / TotalTime)
    
    // We cast to 'float' for decimal precision
    float dutyCycle = (float)highTime / (float)totalTime;
    float degrees   = dutyCycle * 360.0;
    
    // AS5048A PWM frequency is usually ~1kHz (1000us period)
    // A slight correction: The datasheet says the output is clamped between
    // roughly 3% and 97% duty cycle for error handling, but this formula
    // is close enough for a raw test.
    
    Serial.print("High: ");
    Serial.print(highTime);
    Serial.print("us | Total: ");
    Serial.print(totalTime);
    Serial.print("us | Angle: ");
    Serial.println(degrees, 2);
  }

  delay(200); // Update 5 times a second
}