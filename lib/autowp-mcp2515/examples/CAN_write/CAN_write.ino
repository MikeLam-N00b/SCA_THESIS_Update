#include <SPI.h>
#include <mcp2515.h>

// ================== MCP2515 (ESP32-S3 Default SPI Pins) ==================
// ESP32-S3 SPI Default Pins:
// SCK  = GPIO 12
// MISO = GPIO 13
// MOSI = GPIO 11
// CS   = GPIO 10

#define CAN_CS    10
#define MCP_CLOCK MCP_8MHZ   // đổi thành MCP_16MHZ nếu module bạn là 16MHz

MCP2515 mcp2515(CAN_CS);
struct can_frame canMsg;


void setup() {
  Serial.begin(115200);
  delay(1000);  // Đợi Serial khởi động
  
  Serial.println("=== CAN Write Test ===");
  
  // Khởi tạo SPI với các chân mặc định của ESP32-S3
  SPI.begin();
  
  pinMode(CAN_CS, OUTPUT);
  digitalWrite(CAN_CS, HIGH);
  
  Serial.println("Initializing MCP2515...");
  
  // Frame thực tế để gửi lên ô tô
  canMsg.can_id  = 0x100;
  canMsg.can_dlc = 8;
  canMsg.data[0] = 0x31;
  canMsg.data[1] = 0x18;
  canMsg.data[2] = 0x00;
  canMsg.data[3] = 0x00;
  canMsg.data[4] = 0x48;
  canMsg.data[5] = 0x20;
  canMsg.data[6] = 0x00;
  canMsg.data[7] = 0x00;
  
  mcp2515.reset();
  
  MCP2515::ERROR setBitrateResult = mcp2515.setBitrate(CAN_100KBPS, MCP_CLOCK);
  if (setBitrateResult != MCP2515::ERROR_OK) {
    Serial.print("ERROR: setBitrate failed! Code: ");
    Serial.println(setBitrateResult);
  } else {
    Serial.println("Bitrate set successfully");
  }
  
  // Normal mode để gửi thực tế lên bus CAN của ô tô
  MCP2515::ERROR setModeResult = mcp2515.setNormalMode();
  if (setModeResult != MCP2515::ERROR_OK) {
    Serial.print("ERROR: setNormalMode failed! Code: ");
    Serial.println(setModeResult);
  } else {
    Serial.println("Normal mode set successfully");
  }
  
  Serial.println("NOTE: Sending real CAN message to car bus");
  Serial.println("Setup complete!");
}

void loop() {
  MCP2515::ERROR result = mcp2515.sendMessage(&canMsg);
  if (result == MCP2515::ERROR_OK) {
    Serial.println("[OK] STD 0x100 DLC:8 DATA: 31 18 00 00 48 20 00 00");
  } else {
    Serial.print("ERROR sending message: ");
    Serial.println(result);
  }
  
  delay(100);  // Gửi mỗi 100ms
}
