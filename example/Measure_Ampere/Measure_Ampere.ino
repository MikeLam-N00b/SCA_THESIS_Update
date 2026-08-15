#include <Arduino.h>
#include <Wire.h>
#include <INA226.h>

#define SDA_PIN  8
#define SCL_PIN  9

INA226 ina(0x40);

// Deadband: bỏ qua nhiễu khi dòng quá nhỏ
#define CURRENT_DEADBAND_mA  1.0

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  if (!ina.begin()) {
    Serial.println("INA226 not found! Check wiring.");
    while (1);
  }

  // Shunt 0.1Ω (ký hiệu R100 trên CJMCU), max 500mA
  // Nếu shunt khác thì đổi tham số đầu tiên của setMaxCurrentShunt
  ina.setMaxCurrentShunt(0.5, 0.1);

  // Hardware averaging 64 mẫu → giảm nhiễu đáng kể
  ina.setAverage(4);  // 0=1, 1=4, 2=16, 3=64, 4=128, 5=256, 6=512, 7=1024

  Serial.println("INA226 Ready | Shunt=0.1Ω | Max=500mA | Avg=128");
  Serial.println("Voltage(V) | Current(mA) | Power(mW)");
}

void loop() {
  float voltage_V  = ina.getBusVoltage();
  float current_mA = ina.getCurrent() * 1000.0;
  float power_mW   = ina.getPower() * 1000.0;

  // Suppress nhiễu khi không có tải
  if (abs(current_mA) < CURRENT_DEADBAND_mA) current_mA = 0.0;
  if (voltage_V < 0.01) voltage_V = 0.0;

  Serial.printf("%.2f mA\n", current_mA);
  delay(500);
}
