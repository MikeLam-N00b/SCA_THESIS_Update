//#include <Arduino.h>

// LƯU Ý: SPI.h / can_commands.h PHẢI được include TRƯỚC các header NFC.
// Ndef.h định nghĩa lại macro NULL thành (void*)0 — sẽ làm hỏng SPI.h
// (SemaphoreHandle_t paramLock = NULL) nếu được include sau đó.
//
// Các thư viện NFC (PN532, PN532_HSU, NDEF) được vendor ngay trong thư mục
// sketch để tránh xung đột với các thư viện NDEF khác trên máy (ví dụ
// NDEF_MFRC522 có NfcAdapter chỉ hỗ trợ MFRC522). Thư mục sketch nằm ở vị trí
// đầu tiên trong include path nên các header local luôn được ưu tiên.
#include <SPI.h>
#include "can_commands.h"
#include "anchor_config.h"

#include <PN532_HSU.h>
#include <PN532.h>
#include <NfcAdapter.h>
#include <NdefMessage.h>
#include <NdefRecord.h>

// =====================================================
// ESP32 UART2 <-> PN532 HSU
// =====================================================

#define PN532_RX 1
#define PN532_TX 2

// Giới hạn kích thước NDEF text tối đa đọc từ thẻ (tránh VLA quá lớn)
#define MAX_NDEF_TEXT_LEN 128

HardwareSerial PN532Serial(2);

// PN532 HSU
PN532_HSU pn532hsu(PN532Serial);

// QUAN TRỌNG:
// NfcAdapter phải nhận pn532hsu,
// KHÔNG truyền PN532 object.
NfcAdapter nfc(pn532hsu);


// =====================================================
// CONFIG — Giá trị mong đợi (định nghĩa duy nhất)
// =====================================================

// VIN mặc định lấy từ anchor_config.h (VEHICLE_ID)
const char* EXPECTED_VIN = VEHICLE_ID;
const char* EXPECTED_KEY = "9d0b658aa467970a32f315ee018d7307";


// =====================================================
// CAN — globals (giống pattern FreeRTOS_Anchor_noSim)
// =====================================================

static MCP2515*     pMcp2515    = nullptr;
static CANCommands* pCanControl = nullptr;
static bool         carUnlocked = false;   // false = LOCKED, true = UNLOCKED


// =====================================================
// NFC PARSING — Tách VIN và KEY từ NDEF text payload
// =====================================================

// Format mong đợi trong NDEF text (một dòng, cách nhau bởi space):
//   VIN 1HGBH41JXMN109186 KEY 9d0b658aa467970a32f315ee018d7307
// Trả về false nếu thiếu VIN hoặc KEY (dữ liệu không hợp lệ).
bool parseNfcPayload(const String& text, String& vin, String& key)
{
  vin = "";
  key = "";

  // Tách text thành các token (phân cách bởi space/tab/newline)
  String tokens[8];
  int count = 0;
  int pos = 0;
  int len = text.length();

  while (pos < len && count < 8)
  {
    // Bỏ qua khoảng trắng
    while (pos < len)
    {
      char c = text.charAt(pos);
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
      pos++;
    }

    int start = pos;

    // Đọc token
    while (pos < len)
    {
      char c = text.charAt(pos);
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
      pos++;
    }

    if (pos > start)
    {
      tokens[count++] = text.substring(start, pos);
    }
  }

  // Mong đợi đúng 4 token: VIN <vin> KEY <key>
  if (count == 4 && tokens[0] == "VIN" && tokens[2] == "KEY")
  {
    vin = tokens[1];
    key = tokens[3];
  }

  return (vin.length() > 0 && key.length() > 0);
}


// =====================================================
// CAN CONTROL — dùng CANCommands (can_commands.h)
// =====================================================

// LOCK: gửi lệnh khóa, state ép về LOCKED (giống canLock trong anchor_ble.h)
void lockVehicle()
{
  if (pCanControl) pCanControl->lockCar();

  carUnlocked = false;

  Serial.println("Vehicle LOCK command sent");
}

// UNLOCK: gửi lệnh mở khóa, chỉ set state nếu thành công (giống canUnlock)
void unlockVehicle()
{
  if (pCanControl && pCanControl->unlockCar())
  {
    carUnlocked = true;

    Serial.println("Vehicle UNLOCK command sent");
  }
  else
  {
    Serial.println("Vehicle UNLOCK command FAILED");
  }
}


// =====================================================
// NFC VALIDATION — Kiểm tra VIN/KEY và điều khiển xe
// =====================================================

void handleNfcPayload(const String& text)
{
  Serial.println();
  Serial.println("--------------------------------------");
  Serial.println("NFC detected");
  Serial.println("Processing NFC payload");

  String vin, key;

  // -----------------------------------------------
  // Parse VIN + KEY
  // -----------------------------------------------

  if (!parseNfcPayload(text, vin, key))
  {
    Serial.println("NFC payload parsing failed (VIN or KEY missing)");
    Serial.println("NFC TAG: INVALID");

    if (carUnlocked)
    {
      Serial.println("Vehicle is currently UNLOCKED");
      lockVehicle();
    }
    else
    {
      Serial.println("Vehicle is currently LOCKED");
      Serial.println("Keep vehicle LOCKED");
    }

    return;
  }

  Serial.println("VIN parsed successfully");
  Serial.println("KEY parsed successfully");

  // -----------------------------------------------
  // So sánh với giá trị mong đợi
  // -----------------------------------------------

  bool vinValid = (vin == EXPECTED_VIN);
  bool keyValid = (key == EXPECTED_KEY);

  Serial.printf("NFC VIN: %s\n", vinValid ? "VALID" : "INVALID");
  Serial.printf("NFC KEY: %s\n", keyValid ? "VALID" : "INVALID");

  // -----------------------------------------------
  // Quyết định UNLOCK / LOCK
  // -----------------------------------------------

  if (vinValid && keyValid)
  {
    Serial.println("NFC TAG: VALID");

    if (!carUnlocked)
    {
      unlockVehicle();
    }
    else
    {
      Serial.println("Vehicle is currently UNLOCKED - scanning again to LOCK");
      lockVehicle();
    }
  }
  else
  {
    Serial.println("NFC TAG: INVALID");

    if (carUnlocked)
    {
      Serial.println("Vehicle is currently UNLOCKED");
      lockVehicle();
    }
    else
    {
      Serial.println("Vehicle is currently LOCKED");
      Serial.println("Keep vehicle LOCKED");
    }
  }
}


// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("       ESP32 + PN532 HSU");
  Serial.println("       ALWAYS-READ NFC VERIFY");
  Serial.println("======================================");

  // UART2
  PN532Serial.begin(
    115200,
    SERIAL_8N1,
    PN532_RX,
    PN532_TX
  );

  delay(500);

  // Khoi dong NDEF
  nfc.begin();

  Serial.println();
  Serial.println("PN532 da khoi dong.");
  Serial.println("Dat the len PN532 de xac thuc...");

  // =================================================
  // Khoi dong CAN (pattern giống FreeRTOS_Anchor_noSim)
  // =================================================

  SPI.begin();

  pMcp2515    = new MCP2515(CAN_CS);
  pCanControl = new CANCommands(pMcp2515);

  if (!pCanControl->initialize(CAN_CS, CAN_100KBPS, MCP_CLOCK))
  {
    Serial.println("CAN: init failed - vehicle control disabled");
  }
}


// =====================================================
// LOOP — ALWAYS-READ MODE
// =====================================================

void loop()
{
  // Chờ thẻ xuất hiện
  if (!nfc.tagPresent())
  {
    delay(100);
    return;
  }

  Serial.println();
  Serial.println("Card detected, reading NDEF...");

  NfcTag tag = nfc.read();

  String ndefText;

  if (tag.hasNdefMessage())
  {
    NdefMessage message = tag.getNdefMessage();

    int recordCount = message.getRecordCount();

    for (int i = 0; i < recordCount; i++)
    {
      NdefRecord record = message.getRecord(i);

      if (record.getType() == "T")
      {
        int payloadLength = record.getPayloadLength();

        if (payloadLength > 0 && payloadLength <= MAX_NDEF_TEXT_LEN)
        {
          uint8_t payload[payloadLength];

          record.getPayload(payload);

          // Text Record: byte 0 = status byte,
          // byte 1.. = language code, còn lại = text
          uint8_t statusByte     = payload[0];
          uint8_t languageLength = statusByte & 0x3F;
          int     textStart      = 1 + languageLength;

          for (int j = textStart; j < payloadLength; j++)
          {
            ndefText += (char)payload[j];
          }
        }
      }
    }
  }

  Serial.print("NDEF text: ");
  Serial.println(ndefText);

  // Xác thực VIN/KEY + điều khiển khóa xe
  handleNfcPayload(ndefText);

  Serial.print("Current status: ");
  Serial.println(carUnlocked ? "UNLOCKED" : "LOCKED");

  // Chờ lấy thẻ ra
  Serial.println("Remove card...");

  while (nfc.tagPresent())
  {
    delay(100);
  }

  Serial.println("Card removed. Waiting for next card...");

  delay(200);
}