#include <SPI.h>
#include <mcp2515.h>

// ================== MCP2515 - ESP32-S3 SPI MẶC ĐỊNH ==================
// ESP32-S3 default SPI pins: MOSI=11, MISO=13, SCK=12
#define CAN_CS    9

MCP2515 mcp2515(CAN_CS);
struct can_frame canMsg;

// Chọn clock đúng theo module bạn (HW-184 FEIYANG thường 8MHz)
#define MCP_CLOCK MCP_8MHZ   // đổi thành MCP_16MHZ nếu module bạn là 16MHz

// Auto-scan bitrate
const CAN_SPEED canRates[] = { CAN_500KBPS, CAN_250KBPS, CAN_100KBPS, CAN_125KBPS, CAN_200KBPS };
int canRateIdx = 0;
uint32_t lastRateSwitchMs = 0;  
uint32_t lastAnyFrameMs = 0;

// ================== IN CAN FRAME RA SERIAL (FULL STD/EXT) ==================
void printCanFrame(const struct can_frame &f) {
  bool isExt = (f.can_id & CAN_EFF_FLAG);
  uint32_t id = isExt ? (f.can_id & CAN_EFF_MASK) : (f.can_id & CAN_SFF_MASK);

  Serial.print(isExt ? "EXT  0x" : "STD  0x");

  if (isExt) {
    if (id < 0x10000000) Serial.print("0");
    if (id < 0x01000000) Serial.print("0");
    if (id < 0x00100000) Serial.print("0");
    if (id < 0x00010000) Serial.print("0");
    if (id < 0x00001000) Serial.print("0");
    if (id < 0x00000100) Serial.print("0");
    if (id < 0x00000010) Serial.print("0");
  } else {
    if (id < 0x100) Serial.print("0");
    if (id < 0x10)  Serial.print("0");
  }

  Serial.print(id, HEX);
  Serial.print("  DLC:");
  Serial.print(f.can_dlc);
  Serial.print("  DATA:");
  for (int i = 0; i < f.can_dlc; i++) {
    Serial.print(" ");
    if (f.data[i] < 0x10) Serial.print("0");
    Serial.print(f.data[i], HEX);
  }
  Serial.println();
}

// ================== CAN INIT (set bitrate) ==================
void canApplyBitrate(CAN_SPEED rate) {
  mcp2515.reset();
  delay(10);
  
  // Set bitrate
  MCP2515::ERROR result = mcp2515.setBitrate(rate, MCP_CLOCK);
  if (result != MCP2515::ERROR_OK) {
    Serial.print("ERROR: setBitrate failed! Code: ");
    Serial.println(result);
    return;
  }
  
  // ===== QUAN TRỌNG: Disable filter để nhận TẤT CẢ message =====
  // RXB0: Nhận tất cả (standard + extended)
  mcp2515.setFilterMask(MCP2515::MASK0, false, 0x00000000);  // Mask = 0 = accept all
  mcp2515.setFilter(MCP2515::RXF0, false, 0x00000000);
  mcp2515.setFilter(MCP2515::RXF1, false, 0x00000000);
  
  // RXB1: Nhận tất cả (standard + extended)  
  mcp2515.setFilterMask(MCP2515::MASK1, false, 0x00000000);
  mcp2515.setFilter(MCP2515::RXF2, false, 0x00000000);
  mcp2515.setFilter(MCP2515::RXF3, false, 0x00000000);
  mcp2515.setFilter(MCP2515::RXF4, false, 0x00000000);
  mcp2515.setFilter(MCP2515::RXF5, false, 0x00000000);
  
  // Set Normal mode
  result = mcp2515.setNormalMode();
  if (result != MCP2515::ERROR_OK) {
    Serial.print("ERROR: setNormalMode failed! Code: ");
    Serial.println(result);
  }
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== CAN READER - ESP32-S3 ===");

  // Khởi tạo SPI (dùng SPI mặc định của ESP32-S3)
  SPI.begin();
  
  pinMode(CAN_CS, OUTPUT);
  digitalWrite(CAN_CS, HIGH);

  Serial.println("=== CAN MONITOR START (auto bitrate scan) ===");
  Serial.print("MCP CLOCK = ");
  Serial.println((MCP_CLOCK == MCP_8MHZ) ? "8MHz" : "16MHz");
  Serial.println("Initializing MCP2515...");

  canApplyBitrate(canRates[canRateIdx]);
  Serial.print("Starting with bitrate idx: "); 
  Serial.println(canRateIdx);
  Serial.println("Listening for CAN messages...");
  Serial.println("(Auto-scan bitrate every 3s if no frames)\n");

  lastRateSwitchMs = millis();
  lastAnyFrameMs = millis();
}

// ================== LOOP ==================
void loop() {
  // ===== CAN READ =====
  if (mcp2515.readMessage(&canMsg) == MCP2515::ERROR_OK) {
    // Nếu là frame đầu tiên sau khi switch bitrate
    if (millis() - lastAnyFrameMs > 3000) {
      Serial.print("\n*** CAN BUS DETECTED at ");
      switch(canRates[canRateIdx]) {
        case CAN_500KBPS: Serial.print("500"); break;
        case CAN_250KBPS: Serial.print("250"); break;
        case CAN_100KBPS: Serial.print("100"); break;
        case CAN_125KBPS: Serial.print("125"); break;
        case CAN_200KBPS: Serial.print("200"); break;
      }
      Serial.println(" kbps ***\n");
    }
    lastAnyFrameMs = millis();
    printCanFrame(canMsg);
  }

  // ===== AUTO SCAN BITRATE nếu 3 giây không có frame =====
  if (millis() - lastAnyFrameMs > 3000 && millis() - lastRateSwitchMs > 3000) {
    canRateIdx = (canRateIdx + 1) % (sizeof(canRates) / sizeof(canRates[0]));
    Serial.print("\nNo frames for 3s. Switching to bitrate idx ");
    Serial.print(canRateIdx);
    Serial.print(" (");
    switch(canRates[canRateIdx]) {
      case CAN_500KBPS: Serial.print("500"); break;
      case CAN_250KBPS: Serial.print("250"); break;
      case CAN_100KBPS: Serial.print("100"); break;
      case CAN_125KBPS: Serial.print("125"); break;
      case CAN_200KBPS: Serial.print("200"); break;
      default: Serial.print("???"); break;
    }
    Serial.println(" kbps)");
    canApplyBitrate(canRates[canRateIdx]);
    lastRateSwitchMs = millis();
  }

  delay(5);
}
