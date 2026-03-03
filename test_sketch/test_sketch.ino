void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== ESP32 + Antigravity: Connection Test ===");
  Serial.println("If you see this, upload and serial monitor are working!");
  pinMode(2, OUTPUT); // Built-in LED on most ESP32 boards
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
