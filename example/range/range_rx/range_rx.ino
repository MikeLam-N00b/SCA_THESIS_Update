#include "dw3000.h"
#include "SPI.h"

extern SPISettings _fastSPI;  // Can be modified on ESP32 (not const)
extern SPISettings _slowSPI;  // 2MHz SPI - stable for long jumper wires

#define PIN_RST 5
#define PIN_IRQ 4
#define PIN_SS 10

#define RNG_DELAY_MS 100
#define TX_ANT_DLY 16385
#define RX_ANT_DLY 16385
#define ALL_MSG_COMMON_LEN 10
#define ALL_MSG_SN_IDX 2
#define RESP_MSG_POLL_RX_TS_IDX 10
#define RESP_MSG_RESP_TX_TS_IDX 14
#define RESP_MSG_TS_LEN 4
#define POLL_TX_TO_RESP_RX_DLY_UUS 500
#define RESP_RX_TIMEOUT_UUS 2000  // RX timeout (in 1.0256 μs units) = ~2ms
/* Default communication configuration. We use default non-STS DW mode. */
static dwt_config_t config = {
    5,                /* Channel number. */
    DWT_PLEN_128,     /* Preamble length. Used in TX only. */
    DWT_PAC8,         /* Preamble acquisition chunk size. Used in RX only. */
    9,                /* TX preamble code. Used in TX only. */
    9,                /* RX preamble code. Used in RX only. */
    1,                /* 0 to use standard 8 symbol SFD, 1 to use non-standard 8 symbol, 2 for non-standard 16 symbol SFD and 3 for 4z 8 symbol SDF type */
    DWT_BR_6M8,       /* Data rate. */
    DWT_PHRMODE_STD,  /* PHY header mode. */
    DWT_PHRRATE_STD,  /* PHY header rate. */
    (129 + 8 - 8),    /* SFD timeout (preamble length + 1 + SFD length - PAC size). Used in RX only. */
    DWT_STS_MODE_OFF, /* STS disabled */
    DWT_STS_LEN_64,   /* STS length see allowed values in Enum dwt_sts_lengths_e */
    DWT_PDOA_M0       /* PDOA mode off */
};

static uint8_t tx_poll_msg[] = {0x41, 0x88, 0, 0xCA, 0xDE, 'W', 'A', 'V', 'E', 0xE0, 0, 0};
static uint8_t rx_resp_msg[] = {0x41, 0x88, 0, 0xCA, 0xDE, 'V', 'E', 'W', 'A', 0xE1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static uint8_t frame_seq_nb = 0;
static uint8_t rx_buffer[20];
static uint32_t status_reg = 0;
static double tof;
static double distance;
static bool uwb_enabled = true;  // Trạng thái UWB: true = active, false = sleep mode
extern dwt_txconfig_t txconfig_options;
extern dwt_txconfig_t txconfig_options_ch9;

void setup()
{
  UART_init();
  
  // *** FIX SPI CRC ERROR ***
  // Reduce SPI speed from 8MHz to 1MHz for stable communication
  // Long jumper wires + breadboard can cause CRC errors at high speed
  _fastSPI = SPISettings(1000000L, MSBFIRST, SPI_MODE0);  // 1MHz - very stable
  Serial.println("*** SPI speed reduced to 1MHz to fix CRC errors ***");

  spiBegin(PIN_IRQ, PIN_RST);
  spiSelect(PIN_SS);

  delay(2); // Time needed for DW3000 to start up (transition from INIT_RC to IDLE_RC, or could wait for SPIRDY event)

  while (!dwt_checkidlerc()) // Need to make sure DW IC is in IDLE_RC before proceeding
  {
    UART_puts("IDLE FAILED\r\n");
    while (1)
      ;
  }

  if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR)
  {
    UART_puts("INIT FAILED\r\n");
    while (1)
      ;
  }

  // Enabling LEDs here for debug so that for each TX the D1 LED will flash on DW3000 red eval-shield boards.
  dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);

  /* Configure DW IC. See NOTE 6 below. */
  if (dwt_configure(&config)) // if the dwt_configure returns DWT_ERROR either the PLL or RX calibration has failed the host should reset the device
  {
    UART_puts("CONFIG FAILED\r\n");
    while (1)
      ;
  }

  /* Configure the TX spectrum parameters (power, PG delay and PG count) */
  dwt_configuretxrf(&txconfig_options);

  /* Apply default antenna delay value. See NOTE 2 below. */
  dwt_setrxantennadelay(RX_ANT_DLY);
  dwt_settxantennadelay(TX_ANT_DLY);

  /* Set expected response's delay and timeout. See NOTE 1 and 5 below.
   * As this example only handles one incoming frame with always the same delay and timeout, those values can be set here once for all. */
  dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
  dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);

  /* Next can enable TX/RX states output on GPIOs 5 and 6 to help debug, and also TX/RX LEDs
   * Note, in real low power applications the LEDs should not be used. */
  dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);
  
  // *** FIX: Warm up SPI communication after init ***
  // First init may have stale/corrupt data in status register
  Serial.println("Warming up SPI communication...");
  
  // Clear ALL status bits to reset chip state
  dwt_write32bitreg(SYS_STATUS_ID, 0xFFFFFFFFUL);
  delay(10);
  
  // Do dummy reads to stabilize SPI
  for (int i = 0; i < 5; i++) {
    uint32_t dummy = dwt_read32bitreg(SYS_STATUS_ID);
    delay(5);
  }
  
  // Final clear
  dwt_write32bitreg(SYS_STATUS_ID, 0xFFFFFFFFUL);
  
  uint32_t final_status = dwt_read32bitreg(SYS_STATUS_ID);
  Serial.print("SPI warmed up. Status: 0x");
  Serial.println(final_status, HEX);
  
  if (final_status & 0x4) {
    Serial.println("WARNING: SPI CRC error persists! Check hardware:");
    Serial.println("  - Use shorter jumper wires");
    Serial.println("  - Add 10uF capacitor on DW3000 VDD pin");
    Serial.println("  - Check power supply stability");
  }

  Serial.println("Range RX");
  Serial.println("Setup over........");
  
  // Delay ngắn để đảm bảo TX module đã sẵn sàng
  delay(1000);
  Serial.println("Press 'a' to start ranging, 'b' to enter sleep mode...");
}

void loop()
{
  // Kiểm tra nếu có dữ liệu từ Serial
  if (Serial.available() > 0)
  {
    char command = Serial.read();
    
    // Lệnh 'a' hoặc 'A': Bật UWB và đo khoảng cách
    if (command == 'a' || command == 'A')
    {
      if (!uwb_enabled)
      {
        Serial.println("\n=== Waking up from Sleep ===");
        
        // Wake up chip bằng SPI access - chip sẽ tự wake
        delay(5); // Delay ngắn để chip wake up hoàn toàn
        
        // Kiểm tra idle state
        if (!dwt_checkidlerc()) {
          Serial.println("Warning: Chip not in IDLE_RC after wake");
        }
        
        // Cấu hình lại chip sau khi wake
        if (dwt_configure(&config)) {
          Serial.println("ERROR: Reconfigure failed after wake");
          return;
        }
        
        dwt_configuretxrf(&txconfig_options);
        dwt_setrxantennadelay(RX_ANT_DLY);
        dwt_settxantennadelay(TX_ANT_DLY);
        dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
        dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);
        dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);
        
        uwb_enabled = true;
        Serial.println("DW3000 woke up successfully!");
      }
      
      Serial.println("\n=== Starting Range Measurement ===");
      
      // Debug: Kiểm tra hardware trước khi bắt đầu
      Serial.print("Pre-check - IRQ pin: ");
      Serial.println(digitalRead(PIN_IRQ) ? "HIGH" : "LOW");
      
      // Thử tối đa 3 lần nếu bị timeout/error
      bool success = false;
      for (int attempt = 1; attempt <= 3; attempt++)
      {
        if (attempt > 1) {
          Serial.print("\n--- Retry attempt ");
          Serial.print(attempt);
          Serial.println(" ---");
          delay(100); // Delay để chip ổn định giữa các lần thử
        }
        
        if (performRanging()) {
          success = true;
          break;
        }
        // Nếu fail, vòng lặp sẽ tự động retry ngay (tối đa 3 lần)
      }
      
      if (!success) {
        Serial.println("!!! Failed after 3 attempts. Check TX module.");
      }
      
      Serial.println("=== Measurement Complete ===");
      Serial.println("Press 'a' to measure again, 'b' to sleep...\n");
    }
    // Lệnh 'b' hoặc 'B': Tắt UWB và đưa vào chế độ sleep
    else if (command == 'b' || command == 'B')
    {
      if (uwb_enabled)
      {
        Serial.println("\n=== Entering Sleep Mode ===");
        
        // Dừng TX/RX trước
        dwt_forcetrxoff();
        
        // Cấu hình sleep mode: wake on chip select (SPI)
        dwt_configuresleep(DWT_CONFIG, DWT_WAKE_CSN | DWT_SLP_EN);
        
        // Đưa vào sleep mode, sau khi wake sẽ ở trạng thái IDLE_RC
        dwt_entersleep(DWT_DW_IDLE_RC);
        
        uwb_enabled = false;
        Serial.println("DW3000 entered sleep mode!");
        Serial.println("Press 'a' to wake up and measure...\n");
      }
      else
      {
        Serial.println("UWB is already in sleep mode. Press 'a' to wake up.\n");
      }
    }
  }
}

bool performRanging()
{
  if (!uwb_enabled)
  {
    Serial.println("ERROR: UWB is in sleep mode. Press 'a' to wake up.");
    return false;
  }
  
  Serial.print("POLL #");
  Serial.print(frame_seq_nb);
  Serial.print(" - TX...");
  
  /* Clear any previous status bits */
  dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK | SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
  
  /* Write frame data to DW IC and prepare transmission. */
  tx_poll_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
  dwt_writetxdata(sizeof(tx_poll_msg), tx_poll_msg, 0);
  dwt_writetxfctrl(sizeof(tx_poll_msg), 0, 1);
  
  /* Start transmission */
  int tx_ret = dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);
  
  if (tx_ret != DWT_SUCCESS) {
    Serial.print(" FAIL(");
    Serial.print(tx_ret);
    Serial.println(")");
    return false;
  }
  
  /* Wait for TX completion */
  uint32_t tx_timeout = 0;
  uint32_t status_check;
  
  while (!((status_check = dwt_read32bitreg(SYS_STATUS_ID)) & SYS_STATUS_TXFRS_BIT_MASK))
  {
    if (++tx_timeout > 50000) {
      Serial.println(" TX timeout");
      dwt_forcetrxoff();
      return false;
    }
  }
  
  /* Clear TX frame sent event */
  dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);
  
  Serial.print(" RX...");
  
  /* Wait for response */
  uint32_t rx_timeout = 0;
  
  while (!((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) & (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR)))
  {
    if (++rx_timeout > 100000) {
      Serial.println(" RX timeout");
      dwt_forcetrxoff();
      return false;
    }
  }

  frame_seq_nb++;
  
  /* Clear RX good frame event */
  dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);

  if (status_reg & SYS_STATUS_RXFCG_BIT_MASK)
  {
    /* Frame received */
    uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;
    
    if (frame_len <= sizeof(rx_buffer))
    {
      dwt_readrxdata(rx_buffer, frame_len, 0);
      rx_buffer[ALL_MSG_SN_IDX] = 0;
      
      if (memcmp(rx_buffer, rx_resp_msg, ALL_MSG_COMMON_LEN) == 0)
      {
        /* Valid response - calculate distance */
        uint32_t poll_tx_ts = dwt_readtxtimestamplo32();
        uint32_t resp_rx_ts = dwt_readrxtimestamplo32();
        float clockOffsetRatio = (float)dwt_readclockoffset() / (float)(1UL << 26);

        uint32_t poll_rx_ts, resp_tx_ts;
        resp_msg_get_ts(&rx_buffer[RESP_MSG_POLL_RX_TS_IDX], &poll_rx_ts);
        resp_msg_get_ts(&rx_buffer[RESP_MSG_RESP_TX_TS_IDX], &resp_tx_ts);

        int32_t rtd_init = (int32_t)(resp_rx_ts - poll_tx_ts);
        int32_t rtd_resp = (int32_t)(resp_tx_ts - poll_rx_ts);

        tof = (((double)rtd_init - ((double)rtd_resp * (1.0 - (double)clockOffsetRatio))) / 2.0) * DWT_TIME_UNITS;
        distance = tof * SPEED_OF_LIGHT;
        
        Serial.print(" ");
        Serial.print(distance);
        Serial.println(" m");
        
        return true;
      }
    }
    
    Serial.println(" Invalid frame");
    return false;
  }
  else
  {
    /* RX timeout or error */
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
    
    if (status_reg & SYS_STATUS_ALL_RX_TO) {
      Serial.println(" No response (timeout)");
    } else {
      Serial.println(" RX error");
    }
    return false;
  }
}