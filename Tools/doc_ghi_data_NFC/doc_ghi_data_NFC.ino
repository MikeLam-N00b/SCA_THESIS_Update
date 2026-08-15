//#include <Arduino.h>

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

HardwareSerial PN532Serial(2);

// PN532 HSU
PN532_HSU pn532hsu(PN532Serial);

// QUAN TRỌNG:
// NfcAdapter phải nhận pn532hsu,
// KHÔNG truyền PN532 object.
NfcAdapter nfc(pn532hsu);


// =====================================================
// MENU
// =====================================================

void printMenu()
{
  Serial.println();
  Serial.println("======================================");
  Serial.println("        ESP32 + PN532 NDEF");
  Serial.println("======================================");
  Serial.println("1 - DOC NDEF");
  Serial.println("2 - GHI NDEF TEXT");
  Serial.println("3 - DOC UID");
  Serial.println("======================================");
  Serial.println("Nhap lua chon:");
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
  Serial.println("       NDEF READ / WRITE");
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

  printMenu();
}


// =====================================================
// ĐỌC UID
// =====================================================

void readUID()
{
  Serial.println();
  Serial.println("======================================");
  Serial.println("              DOC UID");
  Serial.println("======================================");

  Serial.println("Dat the len PN532...");

  // Chờ thẻ
  while (!nfc.tagPresent())
  {
    delay(100);
  }

  NfcTag tag = nfc.read();

  Serial.println();
  Serial.println("THE DA DUOC PHAT HIEN!");

  Serial.print("Loai the: ");
  Serial.println(tag.getTagType());

  Serial.print("UID: ");
  Serial.println(tag.getUidString());

  Serial.println();
  Serial.println("Lay the ra...");

  while (nfc.tagPresent())
  {
    delay(100);
  }

  Serial.println("Da lay the ra.");

  printMenu();
}


// =====================================================
// ĐỌC NDEF
// =====================================================

void readNDEF()
{
  Serial.println();
  Serial.println("======================================");
  Serial.println("              DOC NDEF");
  Serial.println("======================================");

  Serial.println("Dat the len PN532...");

  // Chờ thẻ
  while (!nfc.tagPresent())
  {
    delay(100);
  }

  Serial.println();
  Serial.println("THE DA DUOC PHAT HIEN!");

  // Đọc thông tin tag
  NfcTag tag = nfc.read();

  Serial.println("--------------------------------------");

  Serial.print("Loai the: ");
  Serial.println(tag.getTagType());

  Serial.print("UID: ");
  Serial.println(tag.getUidString());

  Serial.println("--------------------------------------");


  // ===================================================
  // KIỂM TRA NDEF
  // ===================================================

  if (!tag.hasNdefMessage())
  {
    Serial.println();
    Serial.println("THE KHONG CO NDEF!");

    Serial.println();
    Serial.println("Lay the ra...");

    while (nfc.tagPresent())
    {
      delay(100);
    }

    printMenu();

    return;
  }


  Serial.println();
  Serial.println("THE CO NDEF!");

  // Lấy NDEF message
  NdefMessage message = tag.getNdefMessage();

  int recordCount = message.getRecordCount();

  Serial.print("So NDEF Record: ");
  Serial.println(recordCount);


  // ===================================================
  // ĐỌC TỪNG RECORD
  // ===================================================

  for (int i = 0; i < recordCount; i++)
  {
    NdefRecord record = message.getRecord(i);

    Serial.println();
    Serial.println("======================================");
    Serial.print("NDEF RECORD ");
    Serial.println(i + 1);
    Serial.println("======================================");


    // -------------------------------------------------
    // TNF
    // -------------------------------------------------

    Serial.print("TNF: ");
    Serial.println(record.getTnf());


    // -------------------------------------------------
    // TYPE
    // -------------------------------------------------

    Serial.print("Type: ");
    Serial.println(record.getType());


    // -------------------------------------------------
    // PAYLOAD LENGTH
    // -------------------------------------------------

    int payloadLength =
      record.getPayloadLength();

    Serial.print("Payload length: ");
    Serial.println(payloadLength);


    // -------------------------------------------------
    // PAYLOAD
    // -------------------------------------------------

    if (payloadLength > 0)
    {
      uint8_t payload[payloadLength];

      record.getPayload(payload);


      // ===============================================
      // In HEX
      // ===============================================

      Serial.print("Payload HEX: ");

      for (int j = 0; j < payloadLength; j++)
      {
        if (payload[j] < 0x10)
        {
          Serial.print("0");
        }

        Serial.print(payload[j], HEX);
        Serial.print(" ");
      }

      Serial.println();


      // ===============================================
      // Nếu là NDEF Text Record
      // ===============================================

      Serial.print("Text: ");

      String type = record.getType();

      if (type == "T")
      {
        /*
         * Text Record format:
         *
         * Byte 0:
         *   bit 7 = UTF-16/UTF-8
         *   bit 6..0 = language code length
         *
         * Byte 1..:
         *   Language code
         *
         * Các byte còn lại:
         *   Text
         */

        if (payloadLength >= 1)
        {
          uint8_t statusByte = payload[0];

          uint8_t languageLength =
            statusByte & 0x3F;

          int textStart =
            1 + languageLength;


          if (textStart < payloadLength)
          {
            for (
              int j = textStart;
              j < payloadLength;
              j++
            )
            {
              Serial.print(
                (char)payload[j]
              );
            }
          }
          else
          {
            Serial.print(
              "Khong doc duoc Text"
            );
          }
        }
      }
      else
      {
        // Không phải Text Record
        for (int j = 0; j < payloadLength; j++)
        {
          if (
            payload[j] >= 32 &&
            payload[j] <= 126
          )
          {
            Serial.print(
              (char)payload[j]
            );
          }
          else
          {
            Serial.print(".");
          }
        }
      }

      Serial.println();
    }

    Serial.println("======================================");
  }


  Serial.println();
  Serial.println("DOC NDEF THANH CONG!");


  // ===================================================
  // CHỜ LẤY THẺ RA
  // ===================================================

  Serial.println();
  Serial.println("Lay the ra khoi PN532...");

  while (nfc.tagPresent())
  {
    delay(100);
  }

  Serial.println("Da lay the ra.");

  printMenu();
}


// =====================================================
// GHI NDEF TEXT
// =====================================================

void writeNDEF()
{
  Serial.println();
  Serial.println("======================================");
  Serial.println("             GHI NDEF");
  Serial.println("======================================");

  Serial.println();
  Serial.println("Nhap noi dung muon ghi:");
  Serial.println("(Vi du: hello esp32)");

  // ===================================================
  // Chờ nhập Serial
  // ===================================================

  while (!Serial.available())
  {
    delay(10);
  }

  String text =
    Serial.readStringUntil('\n');

  text.trim();


  // ===================================================
  // Kiểm tra dữ liệu
  // ===================================================

  if (text.length() == 0)
  {
    Serial.println();
    Serial.println("LOI: Noi dung rong!");

    printMenu();

    return;
  }


  Serial.println();
  Serial.println("--------------------------------------");
  Serial.print("Du lieu muon ghi: ");
  Serial.println(text);
  Serial.println("--------------------------------------");


  // ===================================================
  // Tạo NDEF Message
  // ===================================================

  NdefMessage message;

  // Tạo Text Record
  message.addTextRecord(
    text.c_str()
  );


  Serial.println();
  Serial.println("Da tao NDEF Text Record.");


  // ===================================================
  // Chờ thẻ
  // ===================================================

  Serial.println();
  Serial.println(">>> DAT THE LEN PN532 <<<");

  while (!nfc.tagPresent())
  {
    delay(100);
  }


  Serial.println();
  Serial.println("THE DA DUOC PHAT HIEN!");


  // ===================================================
  // Đọc thông tin thẻ
  // ===================================================

  NfcTag tag = nfc.read();

  Serial.print("Loai the: ");
  Serial.println(tag.getTagType());

  Serial.print("UID: ");
  Serial.println(tag.getUidString());


  // ===================================================
  // GHI NDEF
  // ===================================================

  Serial.println();
  Serial.println("Dang ghi NDEF...");


  bool success =
    nfc.write(message);


  // ===================================================
  // Kết quả
  // ===================================================

  if (success)
  {
    Serial.println();
    Serial.println("======================================");
    Serial.println("       GHI NDEF THANH CONG!");
    Serial.println("======================================");

    Serial.print("Du lieu da ghi: ");
    Serial.println(text);

    Serial.println();
    Serial.println("Ban co the dung NFC Tools");
    Serial.println("de doc lai du lieu.");
  }
  else
  {
    Serial.println();
    Serial.println("======================================");
    Serial.println("        GHI NDEF THAT BAI!");
    Serial.println("======================================");

    Serial.println();
    Serial.println("Khong ghi duoc NDEF.");
    Serial.println("Kiem tra lai the / Key / NDEF.");
  }


  // ===================================================
  // Chờ lấy thẻ
  // ===================================================

  Serial.println();
  Serial.println("Lay the ra khoi PN532...");

  while (nfc.tagPresent())
  {
    delay(100);
  }

  Serial.println("Da lay the ra.");

  printMenu();
}


// =====================================================
// LOOP
// =====================================================

void loop()
{
  if (Serial.available())
  {
    char command =
      Serial.read();


    // =================================================
    // 1 - READ NDEF
    // =================================================

    if (command == '1')
    {
      readNDEF();
    }


    // =================================================
    // 2 - WRITE NDEF
    // =================================================

    else if (command == '2')
    {
      writeNDEF();
    }


    // =================================================
    // 3 - READ UID
    // =================================================

    else if (command == '3')
    {
      readUID();
    }
  }

  delay(20);
}