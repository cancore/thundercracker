/* -*- mode: C; c-basic-offset: 4; intent-tabs-mode: nil -*-
 *
 * Thundercracker firmware
 *
 * Copyright <c> 2012 Sifteo, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Driver for the Nordic nRF24L01 radio
 */

#ifndef _NRF24L01_H
#define _NRF24L01_H

#include "spi.h"
#include "gpio.h"
#include "radio.h"
#include "factorytest.h"

class NRF24L01 {
public:
    NRF24L01(GPIOPin _ce,
             GPIOPin _irq,
             GPIOPin _csn,
             SPIMaster _spi)
        : irq(_irq), ce(_ce), csn(_csn), spi(_spi),
          txBuffer(NULL, txData + 1), rxBuffer(rxData + 1), txnState(Idle),
          softRetriesLeft(0),
          hardRetries(PacketTransmission::DEFAULT_HARDWARE_RETRIES)
          {}

    static NRF24L01 instance;

    static const unsigned MAX_HW_RETRIES = 15;
    static const uint8_t AUTO_RETRY_DELAY = 0x10;   // 500 us

    void init();
    void beginTransmitting();

    void setChannel(uint8_t ch);
    uint8_t channel();

    void setConstantCarrier(bool enabled, unsigned channel = 0);
    void setPRXMode(bool enabled);

    static void setRfTestEnabled(bool enabled) {
        rfTestModeEnabled = enabled;
    }

    void isr();
    GPIOPin irq;

    uint32_t irqCount;

    enum TransactionState {
        Idle,
        RXStatus,
        RXPayload,
        TXChannel,
        TXAddressTx,
        TXAddressRx,
        TXRfSetup,
        TXSetupRetr,
        TXPayload,
        TXPulseCE
    };

    ALWAYS_INLINE TransactionState state() const {
        return txnState;
    }

 private:
    enum Command {
        CMD_R_REGISTER          = 0x00,
        CMD_W_REGISTER          = 0x20,
        CMD_R_RX_PAYLOAD        = 0x61,
        CMD_W_TX_PAYLOAD        = 0xA0,
        CMD_FLUSH_TX            = 0xE1,
        CMD_FLUSH_RX            = 0xE2,
        CMD_REUSE_TX_PL         = 0xE3,
        CMD_R_RX_PL_WID         = 0x60,
        CMD_W_ACK_PAYLOAD       = 0xA8,
        CMD_W_TX_PAYLOAD_NO_ACK = 0xB0,
        CMD_NOP                 = 0xFF,
    };

    enum Register {
        REG_CONFIG              = 0x00,
        REG_EN_AA               = 0x01,
        REG_EN_RXADDR           = 0x02,
        REG_SETUP_AW            = 0x03,
        REG_SETUP_RETR          = 0x04,
        REG_RF_CH               = 0x05,
        REG_RF_SETUP            = 0x06,
        REG_STATUS              = 0x07,
        REG_OBSERVE_TX          = 0x08,
        REG_RPD                 = 0x09,
        REG_RX_ADDR_P0          = 0x0A,
        REG_RX_ADDR_P1          = 0x0B,
        REG_RX_ADDR_P2          = 0x0C,
        REG_RX_ADDR_P3          = 0x0D,
        REG_RX_ADDR_P4          = 0x0E,
        REG_RX_ADDR_P5          = 0x0F,
        REG_TX_ADDR             = 0x10,
        REG_RX_PW_P0            = 0x11,
        REG_RX_PW_P1            = 0x12,
        REG_RX_PW_P2            = 0x13,
        REG_RX_PW_P3            = 0x14,
        REG_RX_PW_P4            = 0x15,
        REG_RX_PW_P5            = 0x16,
        REG_FIFO_STATUS         = 0x17,
        REG_DYNPD               = 0x1C,
        REG_FEATURE             = 0x1D,
    };

    enum Status {
        RX_DR                   = 1 << 6,
        TX_DS                   = 1 << 5,
        MAX_RT                  = 1 << 4,
    };

    GPIOPin ce;
    GPIOPin csn;
    SPIMaster spi;

    PacketTransmission txBuffer;
    PacketBuffer rxBuffer;

    // volatile primarily so that factorytest can poll on this, while it
    // may only get updated from ISR context
    volatile TransactionState txnState;

    /*
     * Current retry counts.
     */
    uint8_t softRetriesLeft;
    // Cached device settings to avoid unnecessary SPI comms
    uint8_t hardRetries;
    uint8_t transmitPower;

    /*
     * The extra byte here is required for the SPI command byte that must
     * precede the payload data.
     */
    uint8_t txData[PacketBuffer::MAX_LEN + 1];
    uint8_t rxData[PacketBuffer::MAX_LEN + 1];

    /*
     * NOTE: This exists because the RadioAddress struct does not provide room
     * for the extra byte we need to efficiently transmit these details via DMA.
     *
     * It's cheaper RAM-wise to maintain this single buffer than to extend
     * RadioAddress, which is stored in each Cube object, at the cost of a bit
     * of CPU to copy the data.
     */
    uint8_t txAddressBuffer[sizeof(RadioAddress::id) + 1];

    void handleTimeout();
    void beginReceive();
    void pulseCE();

    /*
     * Helpers to forward RF events to the appropriate destination.
     */

    static bool rfTestModeEnabled;

    static void ALWAYS_INLINE timeout() {
#ifdef RFTEST_GOLD_MASTER
            FactoryTest::timeout();
#else
        if (rfTestModeEnabled)
            FactoryTest::timeout();
        else
            RadioManager::timeout();
#endif
    }

    static void ALWAYS_INLINE ackEmpty(unsigned retries) {
#ifdef RFTEST_GOLD_MASTER
            FactoryTest::ackEmpty(retries);
#else
        if (rfTestModeEnabled)
            FactoryTest::ackEmpty(retries);
        else
            RadioManager::ackEmpty(retries);
#endif
    }

    static void ALWAYS_INLINE ackWithPacket(const PacketBuffer &packet, unsigned retries) {
#ifdef RFTEST_GOLD_MASTER
            FactoryTest::ackWithPacket(packet, retries);
#else
        if (rfTestModeEnabled)
            FactoryTest::ackWithPacket(packet, retries);
        else
            RadioManager::ackWithPacket(packet, retries);
#endif
    }

    static void ALWAYS_INLINE produce(PacketTransmission &tx) {
#ifdef RFTEST_GOLD_MASTER
            FactoryTest::produce(tx);
#else
        if (rfTestModeEnabled)
            FactoryTest::produce(tx);
        else
            RadioManager::produce(tx);
#endif
    }

    unsigned ALWAYS_INLINE retryCount() const {

        /*
         * Return the number of retries expended for the current transmission.
         *
         * This is quantized to `hardRetries` to avoid generating additional
         * traffic to the RF hardware to get the exact count.
         */

        return hardRetries * (txBuffer.numSoftwareRetries - softRetriesLeft);
    }

    static void staticSpiCompletionHandler();
    void onSpiComplete();

    ALWAYS_INLINE void spiBegin() {
        csn.setLow();
    }

    ALWAYS_INLINE void spiEnd() {
        csn.setHigh();
    }

    void spiTransferTable(const uint8_t *table);
};

#endif
