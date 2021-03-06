/*
 * This file is part of HiKoB Openlab.
 *
 * HiKoB Openlab is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, version 3.
 *
 * HiKoB Openlab is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with HiKoB Openlab. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2011-2014 HiKoB.
 */

/**
 * \file radio-rf2xx.c
 *         This file contains wrappers around the OpenLab rf2Xx periph layer
 *
 * \author
 *         Antoine Fraboulet <antoine.fraboulet.at.hikob.com>
 *         Damien Hedde <damien.hedde.at.hikob.com>
 */

#include <stdlib.h>
#include <string.h>

#include "platform.h"
#define NO_DEBUG_HEADER
#define LOG_LEVEL LOG_LEVEL_WARNING
#include "debug.h"
#include "periph/rf2xx.h"
#include "periph/rf2xx.h"

#include "contiki.h"
#include "contiki-net.h"
#include "sys/rtimer.h"
#include "dev/leds.h"
/*---------------------------------------------------------------------------*/

#ifndef RF2XX_DEVICE
#error "RF2XX_DEVICE is not defined"
#endif
extern rf2xx_t RF2XX_DEVICE;
#ifndef RF2XX_CHANNEL
#define RF2XX_CHANNEL   11
#endif
#ifndef RX2XX_TX_POWER
#define RF2XX_TX_POWER  PHY_POWER_0dBm
#endif

#define RF2XX_MAX_PAYLOAD 125
static uint8_t tx_buf[RF2XX_MAX_PAYLOAD];
static uint8_t tx_len;

enum rf2xx_state
{
    RF_IDLE = 0,
    RF_BUSY,
    RF_TX,
    RF_TX_DONE,
    RF_LISTEN,
    RF_RX,
    RF_RX_DONE,
    RF_RX_READ,
};
static volatile enum rf2xx_state rf2xx_state;
static volatile int rf2xx_on;
static volatile int cca_pending;

static int read(uint8_t *buf, uint8_t buf_len);
static void listen(void);
static void idle(void);
static void reset(void);
static void restart(void);
static void irq_handler(handler_arg_t arg);

PROCESS(rf2xx_process, "rf2xx driver");

static int rf2xx_wr_on(void);
static int rf2xx_wr_off(void);
static int rf2xx_wr_prepare(const void *, unsigned short);
static int rf2xx_wr_transmit(unsigned short);
static int rf2xx_wr_send(const void *, unsigned short);
static int rf2xx_wr_read(void *, unsigned short);
static int rf2xx_wr_channel_clear(void);
static int rf2xx_wr_receiving_packet(void);
static int rf2xx_wr_pending_packet(void);

/*---------------------------------------------------------------------------*/

static int
rf2xx_wr_init(void)
{
    log_info("rf2xx_wr_init (channel %u)", RF2XX_CHANNEL);

    rf2xx_on = 0;
    cca_pending = 0;
    tx_len = 0;
    rf2xx_state = RF_IDLE;

    reset();
    idle();
    process_start(&rf2xx_process, NULL);

    return 1;
}

/*---------------------------------------------------------------------------*/

/** Prepare the radio with a packet to be sent. */
static int
rf2xx_wr_prepare(const void *payload, unsigned short payload_len)
{
    log_debug("rf2xx_wr_prepare %d",payload_len);

    if (payload_len > RF2XX_MAX_PAYLOAD)
    {
        log_error("payload is too big");
        tx_len = 0;
        return 1;
    }

    tx_len = payload_len;
    memcpy(tx_buf, payload, tx_len);

    return 0;
}

/*---------------------------------------------------------------------------*/

/** Send the packet that has previously been prepared. */
static int
rf2xx_wr_transmit(unsigned short transmit_len)
{
    int ret, flag;
    uint8_t reg;
    rtimer_clock_t time;
    log_info("rf2xx_wr_transmit %d", transmit_len);

    if (tx_len != transmit_len)
    {
        log_error("Length is has changed (was %u now %u)", tx_len, transmit_len);
        return RADIO_TX_ERR;
    }

    // Check state
    platform_enter_critical();
    // critical section ensures
    // no packet reception will be started
    flag = 0;
    switch (rf2xx_state)
    {
        case RF_LISTEN:
            flag = 1;
        case RF_IDLE:
            rf2xx_state = RF_TX;
            break;
        default:
            platform_exit_critical();
            return RADIO_TX_COLLISION;
    }
    platform_exit_critical();

    if (flag)
    {
        idle();
    }

#ifdef RF2XX_LEDS_ON
    if (transmit_len > 10)
    {
        leds_on(LEDS_RED);
    }
#endif

    // Read IRQ to clear it
    rf2xx_reg_read(RF2XX_DEVICE, RF2XX_REG__IRQ_STATUS);

    // If radio has external PA, enable DIG3/4
    if (rf2xx_has_pa(RF2XX_DEVICE))
    {
        // Enable the PA
        rf2xx_pa_enable(RF2XX_DEVICE);

        // Activate DIG3/4 pins
        reg = rf2xx_reg_read(RF2XX_DEVICE, RF2XX_REG__TRX_CTRL_1);
        reg |= RF2XX_TRX_CTRL_1_MASK__PA_EXT_EN;
        rf2xx_reg_write(RF2XX_DEVICE, RF2XX_REG__TRX_CTRL_1, reg);
    }

    // Wait until PLL ON state
    time = RTIMER_NOW() + RTIMER_SECOND / 1000;
    do
    {
        reg = rf2xx_get_status(RF2XX_DEVICE);

        // Check for block
        if (RTIMER_CLOCK_LT(time, RTIMER_NOW()))
        {
            log_error("Failed to enter tx");
            restart();
            return RADIO_TX_ERR;
        }
    } while (reg != RF2XX_TRX_STATUS__PLL_ON);

    // Enable IRQ interrupt
    rf2xx_irq_enable(RF2XX_DEVICE);

    // Copy the packet to the radio FIFO
    rf2xx_fifo_write_first(RF2XX_DEVICE, tx_len + 2);
    rf2xx_fifo_write_remaining_async(RF2XX_DEVICE, tx_buf,
            tx_len, NULL, NULL);

    // Start TX
    rf2xx_slp_tr_set(RF2XX_DEVICE);

    // Wait until the end of the packet
    while (rf2xx_state == RF_TX)
    {
        ;
    }

    ret = (rf2xx_state == RF_TX_DONE) ? RADIO_TX_OK : RADIO_TX_ERR;

#ifdef RF2XX_LEDS_ON
    leds_off(LEDS_RED);
#endif

    restart();
    return ret;
}

/*---------------------------------------------------------------------------*/

/** Prepare & transmit a packet. */
static int
rf2xx_wr_send(const void *payload, unsigned short payload_len)
{
    log_debug("rf2xx_wr_send %d", payload_len);
    if (rf2xx_wr_prepare(payload, payload_len))
    {
        return RADIO_TX_ERR;
    }
    return rf2xx_wr_transmit(payload_len);
}

/*---------------------------------------------------------------------------*/

/** Read a received packet into a buffer. */
static int
rf2xx_wr_read(void *buf, unsigned short buf_len)
{
    int len;
    log_info("rf2xx_wr_read %d", buf_len);

    // Is there a packet pending
    platform_enter_critical();
    if (rf2xx_state != RF_RX_DONE)
    {
        platform_exit_critical();
        return 0;
    }
    rf2xx_state = RF_RX_READ;
    platform_exit_critical();

    // Get the packet
    len = read(buf, buf_len);

    restart();
    return len;
}

/*---------------------------------------------------------------------------*/

/** Perform a Clear-Channel Assessment (CCA) to find out if there is
    a packet in the air or not. */
static int
rf2xx_wr_channel_clear(void)
{
    int clear = 1;
    log_debug("rf2xx_wr_channel_clear");

    // critical section is necessary
    // to avoid spi access conflicts
    // with irq_handler
    switch (rf2xx_state)
    {
        uint8_t reg;
        case RF_LISTEN:
            //initiate a cca request
            platform_enter_critical();
            reg = RF2XX_PHY_CC_CCA_DEFAULT__CCA_MODE |
                (RF2XX_CHANNEL & RF2XX_PHY_CC_CCA_MASK__CHANNEL) |
                RF2XX_PHY_CC_CCA_MASK__CCA_REQUEST;
            rf2xx_reg_write(RF2XX_DEVICE, RF2XX_REG__PHY_CC_CCA, reg);
            platform_exit_critical();

            // wait cca to be done
            do
            {
                platform_enter_critical();
                reg = rf2xx_reg_read(RF2XX_DEVICE, RF2XX_REG__TRX_STATUS);
                platform_exit_critical();
            }
            while (rf2xx_state == RF_LISTEN && !(reg & RF2XX_TRX_STATUS_MASK__CCA_DONE));

            // get result
            if (!(reg & RF2XX_TRX_STATUS_MASK__CCA_STATUS))
            {
                clear = 0;
            }
            break;

        case RF_RX:
            clear = 0;
            break;

        default:
            break;
    }

    return clear;
}

/*---------------------------------------------------------------------------*/

/** Check if the radio driver is currently receiving a packet */
static int
rf2xx_wr_receiving_packet(void)
{
    return (rf2xx_state == RF_RX) ? 1 : 0;
}

/*---------------------------------------------------------------------------*/

/** Check if the radio driver has just received a packet */
static int
rf2xx_wr_pending_packet(void)
{
    return (rf2xx_state == RF_RX_DONE) ? 1 : 0;
}

/*---------------------------------------------------------------------------*/

/** Turn the radio on. */
static int
rf2xx_wr_on(void)
{
    int flag = 0;
    log_debug("rf2xx_wr_on");

    platform_enter_critical();
    if (!rf2xx_on)
    {
        rf2xx_on = 1;
        if (rf2xx_state == RF_IDLE)
        {
            flag = 1;
            rf2xx_state == RF_BUSY;
        }
    }
    platform_exit_critical();

    if (flag)
    {
        listen();
    }
    return 1;
}

/*---------------------------------------------------------------------------*/

/** Turn the radio off. */
static int
rf2xx_wr_off(void)
{
    int flag = 0;
    log_debug("rf2xx_wr_off");

    platform_enter_critical();
    if (rf2xx_on)
    {
        rf2xx_on = 0;
        if (rf2xx_state == RF_LISTEN)
        {
            flag = 1;
            rf2xx_state = RF_BUSY;
        }
    }
    platform_exit_critical();

    if (flag)
    {
        idle();
        rf2xx_state = RF_IDLE;
    }
    return 1;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

const struct radio_driver rf2xx_driver =
  {
    .init             = rf2xx_wr_init,
    .prepare          = rf2xx_wr_prepare,
    .transmit         = rf2xx_wr_transmit,
    .send             = rf2xx_wr_send,
    .read             = rf2xx_wr_read,
    .channel_clear    = rf2xx_wr_channel_clear,
    .receiving_packet = rf2xx_wr_receiving_packet,
    .pending_packet   = rf2xx_wr_pending_packet,
    .on               = rf2xx_wr_on,
    .off              = rf2xx_wr_off,
 };

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

PROCESS_THREAD(rf2xx_process, ev, data)
{
    PROCESS_BEGIN();

    while(1)
    {
        static int len;
        static int flag;
        PROCESS_YIELD_UNTIL(ev == PROCESS_EVENT_POLL);

        /*
         * at this point, we may be in any state
         *
         * this process can be 'interrupted' by rtimer tasks
         * such as contikimac rdc listening task
         * which may call on/off/read/receiving/pending
         */

        len = 0;
        flag = 0;
        platform_enter_critical();
        if (rf2xx_state == RF_RX_DONE)
        {
            // the process will do the read
            rf2xx_state = RF_RX_READ;
            flag = 1;
        }
        platform_exit_critical();

        if (flag)
        {
            // get data
            packetbuf_clear();
            len = read(packetbuf_dataptr(), PACKETBUF_SIZE - PACKETBUF_HDR_SIZE);

            restart();

            // eventually call upper layer
            if (len > 0)
            {
                packetbuf_set_datalen(len);
                NETSTACK_RDC.input();
            }
        }
    }

    PROCESS_END();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

static void reset(void)
{
    uint8_t reg;
    // Stop any Asynchronous access
    rf2xx_fifo_access_cancel(RF2XX_DEVICE);

    // Configure the radio interrupts
    rf2xx_irq_disable(RF2XX_DEVICE);
    rf2xx_irq_configure(RF2XX_DEVICE, irq_handler, NULL);

    // Disable DIG2 pin
    if (rf2xx_has_dig2(RF2XX_DEVICE))
    {
        rf2xx_dig2_disable(RF2XX_DEVICE);
    }

    // Reset the SLP_TR output
    rf2xx_slp_tr_clear(RF2XX_DEVICE);

    // Reset the radio chip
    rf2xx_reset(RF2XX_DEVICE);

    // Enable Dynamic Frame Buffer Protection, standard data rate (250kbps)
    reg = RF2XX_TRX_CTRL_2_MASK__RX_SAFE_MODE;
    rf2xx_reg_write(RF2XX_DEVICE, RF2XX_REG__TRX_CTRL_2, reg);

    // Set max TX power
    reg = RF2XX_PHY_TX_PWR_DEFAULT__PA_BUF_LT
            | RF2XX_PHY_TX_PWR_DEFAULT__PA_LT
            | RF2XX_PHY_TX_PWR_TX_PWR_VALUE__3dBm;
    rf2xx_reg_write(RF2XX_DEVICE, RF2XX_REG__PHY_TX_PWR, reg);

    // Disable CLKM signal
    reg = RF2XX_TRX_CTRL_0_DEFAULT__PAD_IO
            | RF2XX_TRX_CTRL_0_DEFAULT__PAD_IO_CLKM
            | RF2XX_TRX_CTRL_0_DEFAULT__CLKM_SHA_SEL
            | RF2XX_TRX_CTRL_0_CLKM_CTRL__OFF;
    rf2xx_reg_write(RF2XX_DEVICE, RF2XX_REG__TRX_CTRL_0, reg);

    /** Set XCLK TRIM
     * \todo this highly depends on the board
     */
    reg = RF2XX_XOSC_CTRL__XTAL_MODE_CRYSTAL | 0x0;
    rf2xx_reg_write(RF2XX_DEVICE, RF2XX_REG__XOSC_CTRL, reg);

    // Set channel
    reg = RF2XX_PHY_CC_CCA_DEFAULT__CCA_MODE |
        (RF2XX_CHANNEL & RF2XX_PHY_CC_CCA_MASK__CHANNEL);
    rf2xx_reg_write(RF2XX_DEVICE, RF2XX_REG__PHY_CC_CCA, reg);

    // Set IRQ to TRX END/RX_START/CCA_DONE
    rf2xx_reg_write(RF2XX_DEVICE, RF2XX_REG__IRQ_MASK,
            RF2XX_IRQ_STATUS_MASK__TRX_END |
            RF2XX_IRQ_STATUS_MASK__RX_START);
}

/*---------------------------------------------------------------------------*/

static void idle(void)
{
    // Disable the interrupts
    rf2xx_irq_disable(RF2XX_DEVICE);

    // Cancel any ongoing transfer
    rf2xx_fifo_access_cancel(RF2XX_DEVICE);

    // Clear slp/tr
    rf2xx_slp_tr_clear(RF2XX_DEVICE);

    // Force IDLE
    rf2xx_set_state(RF2XX_DEVICE, RF2XX_TRX_STATE__FORCE_PLL_ON);

    // If radio has external PA, disable DIG3/4
    if (rf2xx_has_pa(RF2XX_DEVICE))
    {
        // Enable the PA
        rf2xx_pa_disable(RF2XX_DEVICE);

        // De-activate DIG3/4 pins
        uint8_t reg = rf2xx_reg_read(RF2XX_DEVICE, RF2XX_REG__TRX_CTRL_1);
        reg &= ~RF2XX_TRX_CTRL_1_MASK__PA_EXT_EN;
        rf2xx_reg_write(RF2XX_DEVICE, RF2XX_REG__TRX_CTRL_1, reg);
    }
}

/*---------------------------------------------------------------------------*/

static void listen(void)
{
    uint8_t reg;
    rtimer_clock_t time;

    // Read IRQ to clear it
    rf2xx_reg_read(RF2XX_DEVICE, RF2XX_REG__IRQ_STATUS);

    // If radio has external PA, enable DIG3/4
    if (rf2xx_has_pa(RF2XX_DEVICE))
    {
        // Enable the PA
        rf2xx_pa_enable(RF2XX_DEVICE);

        // Activate DIG3/4 pins
        reg = rf2xx_reg_read(RF2XX_DEVICE, RF2XX_REG__TRX_CTRL_1);
        reg |= RF2XX_TRX_CTRL_1_MASK__PA_EXT_EN;
        rf2xx_reg_write(RF2XX_DEVICE, RF2XX_REG__TRX_CTRL_1, reg);
    }

    // Enable IRQ interrupt
    rf2xx_irq_enable(RF2XX_DEVICE);

    // Start RX
    platform_enter_critical();
    rf2xx_state = RF_LISTEN;
    rf2xx_set_state(RF2XX_DEVICE, RF2XX_TRX_STATE__RX_ON);
    platform_exit_critical();
}

/*---------------------------------------------------------------------------*/

static void restart(void)
{
    idle();

    if (rf2xx_on)
    {
        listen();
    }
    else
    {
        rf2xx_state = RF_IDLE;
    }
}

/*---------------------------------------------------------------------------*/

static int read(uint8_t *buf, uint8_t buf_len)
{
    uint8_t len;
    // Check the CRC is good
    if (!(rf2xx_reg_read(RF2XX_DEVICE, RF2XX_REG__PHY_RSSI)
            & RF2XX_PHY_RSSI_MASK__RX_CRC_VALID))
    {
        log_warning("Received packet with bad crc");
        return 0;
    }

#ifdef RF2XX_LEDS_ON
        leds_on(LEDS_GREEN);
#endif

    // Get payload length
    len = rf2xx_fifo_read_first(RF2XX_DEVICE) - 2;
    log_info("Received packet of length: %u", len);

    // Check valid length (not zero and enough space to store it)
    if (len > buf_len)
    {
        log_warning("Received packet is too big (%u)", len);
        // Error length, end transfer
        rf2xx_fifo_read_remaining(RF2XX_DEVICE, buf, 0);
        return 0;
    }

    // Read payload
    rf2xx_fifo_read_remaining(RF2XX_DEVICE, buf, len);

#ifdef RF2XX_LEDS_ON
        leds_off(LEDS_GREEN);
#endif

    return len;
}

/*---------------------------------------------------------------------------*/

static void irq_handler(handler_arg_t arg)
{
    (void) arg;
    uint8_t reg;
    int state = rf2xx_state;
    switch (state)
    {
        case RF_TX:
        case RF_LISTEN:
        case RF_RX:
            break;
        default:
            log_warning("unexpected irq while state %d", state);
            // may eventually happen when transitioning
            // from listen to idle for example
            return;
    }

    // only read irq_status in somes states to avoid any
    // concurrency problem on spi access
    reg = rf2xx_reg_read(RF2XX_DEVICE, RF2XX_REG__IRQ_STATUS);

    // rx start detection
    if (reg & RF2XX_IRQ_STATUS_MASK__RX_START && state == RF_LISTEN)
    {
        rf2xx_state = state = RF_RX;
    }

    // rx/tx end
    if (reg & RF2XX_IRQ_STATUS_MASK__TRX_END)
    {
        switch (state)
        {
            case RF_TX:
                rf2xx_state = state = RF_TX_DONE;
                break;
            case RF_RX:
            case RF_LISTEN:
                rf2xx_state = state = RF_RX_DONE;
                // we do not want to start a 2nd RX
                rf2xx_set_state(RF2XX_DEVICE, RF2XX_TRX_STATE__PLL_ON);
                process_poll(&rf2xx_process);
                break;
        }
    }
}

