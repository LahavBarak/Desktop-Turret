void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }  // Wait for host to open port (DTR asserted)
  Serial.println("=== ESP32 Connection Test ===");
  Serial.println("If you see this, upload and serial monitor are working!");
  pinMode(10, OUTPUT); // XIAO ESP32-C3 user LED is GPIO10 (active-low)
}

int counter = 0;

void loop() {
  digitalWrite(2, HIGH);
  Serial.print("Hello from ESP32! Count: ");
  Serial.println(counter++);
  delay(1000);
  digitalWrite(2, LOW);
  delay(1000);
}
