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
 * Copyright (C) 2011,2012 HiKoB.
 */

/**
 * \file openlab-main.c
 *         Configuration for HiKoB OpenLab Fox
 *
 * \author
 *         Antoine Fraboulet <antoine.fraboulet.at.hikob.com>
 *         
 */

#include <string.h>

#include "platform.h"
#include "drivers/unique_id.h"
#define NO_DEBUG_HEADER
//#define LOG_LEVEL LOG_LEVEL_INFO
#include "debug.h"

#include "contiki.h"
#include "lib/sensors.h"
#include "dev/serial-line.h"
#include "dev/watchdog.h"
#include "contiki-net.h"
#include "slip.h"

#ifndef SLIP_ARCH_CONF_ENABLE
#define SLIP_ARCH_CONF_ENABLE 0
#endif

int putchar(int c);
void xputc(char c);

#define PROCESS_CONF_NO_PROCESS_NAMES 0

#if RIMEADDR_SIZE != 8
#error "RIME address size should be set to 8"
#endif /*RIMEADDR_SIZE == 8*/

/*---------------------------------------------------------------------------*/
/*
 * Openlab Fox platform, sensors definition
 *
 */

/** Fox Button **/
extern const struct sensors_sensor button_sensor;

/** Sensors **/
const struct sensors_sensor *sensors[] = {
    &button_sensor,
    0
};

unsigned char sensors_flags[(sizeof(sensors) / sizeof(struct sensors_sensor *))];

/*---------------------------------------------------------------------------*/
void uip_log(char *msg)
{
    log_printf("%s\n", msg);
}
/*---------------------------------------------------------------------------*/
void set_rime_addr()
{
    /* Company 3 Bytes */
    rimeaddr_node_addr.u8[0] = 0x01;
    rimeaddr_node_addr.u8[1] = 0x23;
    rimeaddr_node_addr.u8[2] = 0x45;

    /* Platform identifier */
    rimeaddr_node_addr.u8[3] = 0x00;

    /* Generate 4 remaining bytes using uid of processor */
    int i;
    for (i = 0; i < 4; i++)
    {
        rimeaddr_node_addr.u8[i+4] = uid->uid8[i+6];
    }

    log_debug("Uid: %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
            uid->uid8[0],
            uid->uid8[1],
            uid->uid8[2],
            uid->uid8[3],
            uid->uid8[4],
            uid->uid8[5],
            uid->uid8[6],
            uid->uid8[7],
            uid->uid8[8],
            uid->uid8[9],
            uid->uid8[10],
            uid->uid8[11]);
    log_debug("Rime Addr: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
            rimeaddr_node_addr.u8[0],
            rimeaddr_node_addr.u8[1],
            rimeaddr_node_addr.u8[2],
            rimeaddr_node_addr.u8[3],
            rimeaddr_node_addr.u8[4],
            rimeaddr_node_addr.u8[5],
            rimeaddr_node_addr.u8[6],
            rimeaddr_node_addr.u8[7]);
}
/*---------------------------------------------------------------------------*/
static void
print_processes(struct process * const processes[])
{
#if !PROCESS_CONF_NO_PROCESS_NAMES
    printf(" Starting");
    while(*processes != NULL) 
    {
	printf(" '%s'", (*processes)->name);
	processes++;
    }
#endif /* !PROCESS_CONF_NO_PROCESS_NAMES */
    putchar('\n');
}
/*---------------------------------------------------------------------------*/
static void char_rx(handler_arg_t arg, uint8_t c)
{
    serial_line_input_byte(c);
}
/*---------------------------------------------------------------------------*/
int main()
{
    static uint32_t idle_count = 0;

    /* 
     * OpenLab Platform init
     * 
     */

    platform_init();

    /*
     * Contiki core 
     *
     */

    clock_init();
    process_init();
    rtimer_init();
    process_start(&etimer_process, NULL);
    ctimer_init();

    /* 
     * Sensors 
     *
     * registered sensors: button
     *
     * TODO: add lsm303dlhc + l3g4200 + lps331
     */

    process_start(&sensors_process, NULL);

    /* 
     * Network 
     *
     */

    netstack_init();
    set_rime_addr();

#if UIP_CONF_IPV6
    memcpy(&uip_lladdr.addr, &rimeaddr_node_addr, sizeof(uip_lladdr.addr));
    process_start(&tcpip_process, NULL);

    #if VIZTOOL_CONF_ON
    process_start(&viztool_process, NULL);
    #endif

    #if (!UIP_CONF_IPV6_RPL)
    {
	uip_ipaddr_t ipaddr;

	uip_ip6addr(&ipaddr, 0x2001, 0x630, 0x301, 0x6453, 0, 0, 0, 0);
	uip_ds6_set_addr_iid(&ipaddr, &uip_lladdr);
	uip_ds6_addr_add(&ipaddr, 0, ADDR_TENTATIVE);
    }
    #endif /* UIP_CONF_IPV6_RPL */
#endif /* UIP_CONF_IPV6 */

    /*
     * init serial line
     */
    serial_line_init();
    uart_set_rx_handler(uart_print, char_rx, NULL);

    /*
     * eventually init slip device
     * wich may override serial line
     */
#if SLIP_ARCH_CONF_ENABLE
#ifndef UIP_CONF_LLH_LEN
#error "LLH_LEN is not defined"
#elif UIP_CONF_LLH_LEN != 0
#error "LLH_LEN must be 0 to use slip interface"
#endif
    slip_arch_init(SLIP_ARCH_CONF_BAUDRATE);
#endif

    /*
     * Start
     */
    print_processes(autostart_processes);
    autostart_start(autostart_processes);
    watchdog_start();

    while(1) 
    {
	int r;
	do 
	{
	    watchdog_periodic();
	    r = process_run();
	} while(r > 0);
	idle_count++;
    }

    return 0;
}

