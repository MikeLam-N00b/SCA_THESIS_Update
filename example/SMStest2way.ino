#include <HardwareSerial.h>

#define simSerial Serial2
#define RX_PIN 16
#define TX_PIN 17

#define PHONE_NUMBER "+84938313605"

void waitForResponse() {
  unsigned long t = millis();
  while (millis() - t < 3000) {
    while (simSerial.available()) {
      Serial.write(simSerial.read());
    }
  }
}

void setup() {
  Serial.begin(115200);
  simSerial.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  delay(8000);

  simSerial.println("AT");
  waitForResponse();

  simSerial.println("AT+CMGF=1");
  waitForResponse();

  simSerial.println("AT+CNMI=2,2,0,0,0");
  waitForResponse();

  Serial.println("===== READY =====");
}

void loop() {

  // 📥 Nhận từ SIM
  while (simSerial.available()) {
    Serial.write(simSerial.read());
  }

  // 📤 Gửi SMS hoặc AT
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input.startsWith("AT")) {
      simSerial.println(input);
      waitForResponse();
    }
    else {
      Serial.println("Sending SMS...");

      simSerial.println("AT+CMGF=1");
      waitForResponse();

      simSerial.print("AT+CMGS=\"");
      simSerial.print(PHONE_NUMBER);
      simSerial.println("\"");

      delay(1000); // 🔥 CHỜ dấu >

      simSerial.print(input);
      delay(500);

      simSerial.write(26); // Ctrl+Z

      waitForResponse();

      Serial.println("DONE");
    }
  }
}