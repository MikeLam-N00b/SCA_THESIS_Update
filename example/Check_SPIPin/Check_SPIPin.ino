#include <SPI.h>

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== Default ESP32 SPI Pins ===");
  Serial.print("MOSI: GPIO ");
  Serial.println(MOSI);
  Serial.print("MISO: GPIO ");
  Serial.println(MISO);
  Serial.print("SCK:  GPIO ");
  Serial.println(SCK);
  Serial.print("SS:   GPIO ");
  Serial.println(SS);
}

void loop() {
  // Empty
}