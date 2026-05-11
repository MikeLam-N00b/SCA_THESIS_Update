#pragma once
// anchor_uwb.h — DW3000 UWB hardware init/deinit and SS-TWR responder loop.

#include <SPI.h>
#include "dw3000.h"
#include "anchor_ipc.h"
#include "anchor_config.h"

// Channel 5 | 1024-symbol preamble | PAC 32 | code 9 | 850 kbps | STS mode 1
static dwt_config_t uwbConfig = {
    5, DWT_PLEN_1024, DWT_PAC32, 9, 9, 1,
    DWT_BR_850K, DWT_PHRMODE_STD, DWT_PHRRATE_STD,
    1001, DWT_STS_MODE_1, DWT_STS_LEN_256, DWT_PDOA_M0
};

// STS key/IV — owner mode uses pairingKey; friend mode uses s_activeFriendKey
static dwt_sts_cp_key_t sts_key;
static dwt_sts_cp_iv_t  sts_iv;
static bool stsConfigured = false;

extern dwt_txconfig_t txconfig_options;

// IEEE 802.15.4 frame: ctrl=0x4188, PAN=0xDECA
static uint8_t rx_poll_msg[] = {0x41U,0x88U,0U,0xCAU,0xDEU,'W','A','V','E',0xE0U,0U,0U};
static uint8_t tx_resp_msg[] = {0x41U,0x88U,0U,0xCAU,0xDEU,'V','E','W','A',0xE1U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U};
static uint8_t rx_buffer[MSG_BUFFER_SIZE];
static uint8_t frame_seq_nb = 0U;

static bool initUWB() {
    Serial.println("UWB: initializing...");
    digitalWrite(CAN_CS, HIGH);  // deselect MCP2515 before touching SPI bus
    spiBegin(PIN_IRQ, PIN_RST);
    { extern uint8_t _ss; _ss = PIN_SS; }
    pinMode(PIN_SS, OUTPUT); digitalWrite(PIN_SS, HIGH);

    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    vTaskDelay(pdMS_TO_TICKS(2));
    pinMode(PIN_RST, INPUT);
    vTaskDelay(pdMS_TO_TICKS(50));

    int retries = 500;
    while (!dwt_checkidlerc() && retries-- > 0) vTaskDelay(pdMS_TO_TICKS(1));
    if (retries <= 0) { Serial.println("UWB: IDLE_RC timeout"); goto fail; }
    if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR) { Serial.println("UWB: init failed"); goto fail; }

    dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);
    if (dwt_configure(&uwbConfig) != 0) { Serial.println("UWB: configure failed"); goto fail; }

    dwt_configuretxrf(&txconfig_options);
    dwt_setrxantennadelay(RX_ANT_DLY);
    dwt_settxantennadelay(TX_ANT_DLY);
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

    memcpy(&sts_key, s_uwbFriendMode ? s_activeFriendKey : pairingKey, sizeof(sts_key));
    sts_iv.iv0 = 0x00000001U;
    sts_iv.iv1 = 0x00000000U;
    sts_iv.iv2 = 0x00000000U;
    sts_iv.iv3 = 0x00000000U;
    dwt_configurestskey(&sts_key);
    dwt_configurestsiv(&sts_iv);
    dwt_configurestsloadiv();
    stsConfigured = true;

    Serial.println("UWB: ready (STS mode 1)");
    return true;
fail:
    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    return false;
}

static void deinitUWB() {
    dwt_forcetrxoff();
    dwt_softreset();
    vTaskDelay(pdMS_TO_TICKS(2));
    // Hold DW3000 in RESET so it does not drive MISO, avoiding SPI bus conflict with MCP2515
    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    stsConfigured = false;
    Serial.println("UWB: stopped");
}

// SS-TWR responder — one iteration per call, uses vTaskDelay instead of delay()
static void uwbResponderLoop() {
    dwt_writetodevice(STS_IV0_ID, 0, 4, (uint8_t *)&sts_iv.iv0);
    dwt_configurestsloadiv();

    dwt_rxenable(DWT_START_RX_IMMEDIATE);
    uint32_t status_reg = 0U;
    unsigned long t0 = millis();
    while (!((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) &
             (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR))) {
        if ((millis() - t0) > 100UL) { dwt_forcetrxoff(); return; }
        taskYIELD();
    }
    if (!(status_reg & SYS_STATUS_RXFCG_BIT_MASK)) {
        dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_ERR);
        dwt_forcetrxoff(); return;
    }

    // Reject frame if STS quality invalid (relay attack mitigation)
    int16_t stsQual;
    if (dwt_readstsquality(&stsQual) < 0) {
        dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_GOOD);
        dwt_forcetrxoff(); return;
    }

    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
    uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;
    if (frame_len == 0 || frame_len > sizeof(rx_buffer)) { dwt_forcetrxoff(); return; }

    dwt_readrxdata(rx_buffer, frame_len, 0U);
    rx_buffer[ALL_MSG_SN_IDX] = 0U;
    if (memcmp(rx_buffer, rx_poll_msg, ALL_MSG_COMMON_LEN) != 0) { dwt_forcetrxoff(); return; }

    uint64_t poll_rx_ts   = get_rx_timestamp_u64();
    uint32_t resp_tx_time = (uint32_t)((poll_rx_ts + ((uint64_t)POLL_RX_TO_RESP_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8);

    dwt_forcetrxoff();
    dwt_setdelayedtrxtime(resp_tx_time);
    uint64_t resp_tx_ts = (((uint64_t)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;

    resp_msg_set_ts(&tx_resp_msg[RESP_MSG_POLL_RX_TS_IDX], poll_rx_ts);
    resp_msg_set_ts(&tx_resp_msg[RESP_MSG_RESP_TX_TS_IDX], resp_tx_ts);
    tx_resp_msg[ALL_MSG_SN_IDX] = frame_seq_nb;

    dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0U);
    dwt_writetxfctrl(sizeof(tx_resp_msg), 0U, 1);
    if (dwt_starttx(DWT_START_TX_DELAYED) != DWT_SUCCESS) {
        dwt_forcetrxoff(); return;
    }
    unsigned long tx_t0 = millis();
    while (!(dwt_read32bitreg(SYS_STATUS_ID) & SYS_STATUS_TXFRS_BIT_MASK)) {
        if ((millis() - tx_t0) > 10UL) { dwt_forcetrxoff(); return; }
    }
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);
    frame_seq_nb++;
}
