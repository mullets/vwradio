#include "main.h"
#include "kwp1281.h"
#include "uart.h"
#include <string.h>
#include <avr/io.h>
#include <util/delay.h>

// Report unrecoverable error and halt
void _panic(char *msg)
{
    uart_flush_tx(UART_DEBUG);
    uart_puts(UART_DEBUG, "\n\n*** KWP ERROR: ");
    uart_puts(UART_DEBUG, msg);
    uart_puts(UART_DEBUG, "\n");
    while(1);
}


// Send module address at 5bps
static void _send_address(uint8_t address)
{
    uart_puts(UART_DEBUG, "INIT 0x");
    uart_puthex(UART_DEBUG, address);
    uart_puts(UART_DEBUG, "\n\n");

    UCSR1B &= ~_BV(RXEN1);  // Disable RX (PD2/TXD1)
    UCSR1B &= ~_BV(TXEN1);  // Disable TX (PD3/TXD1)
    DDRD |= _BV(PD3);       // PD3 = output

    uint8_t parity = 1;
    for (uint8_t i=0; i<10; i++) {
        uint8_t bit;
        switch (i) {
            case 0:     // start bit
                bit = 0;
                break;
            case 8:     // parity bit
                bit = parity;
                break;
            case 9:     // stop bit
                bit = 1;
                break;
            default:    // 7 data bits
                bit = (uint8_t)((address & (1 << (i - 1))) != 0);
                parity ^= bit;
        }

        if (bit == 1) {
            PORTD |= _BV(PD3);  // high
        } else {
            PORTD &= ~_BV(PD3); // low
        }
        _delay_ms(200);         // 1000ms / 5bps = 200ms per bit
    }

    UCSR1B |= _BV(TXEN1);   // Enable TX (PD3/TXD1)
    UCSR1B |= _BV(RXEN1);   // Enable RX (PD2/TXD1)
    DDRD &= ~_BV(PD3);      // PD3 = input
}


// Send byte only
static void _send_byte(uint8_t c)
{
    _delay_ms(1);

    uart_blocking_put(UART_KLINE, c);             // send byte
    uint8_t echo = uart_blocking_get(UART_KLINE); // consume its echo
    if (echo != c) { _panic("echo wrong"); }

    uart_puts(UART_DEBUG, "TX: ");
    uart_puthex(UART_DEBUG, c);
    uart_put(UART_DEBUG, '\n');
}


// Send byte and receive its complement
static void _send_byte_recv_compl(uint8_t c)
{
    _send_byte(c);

    uint8_t complement = uart_blocking_get(UART_KLINE);
    if (complement != (c ^ 0xff)) { _panic("rx complement wrong"); }

    uart_puts(UART_DEBUG, "R_: ");
    uart_puthex(UART_DEBUG, complement);
    uart_put(UART_DEBUG, '\n');
}


// Receive byte only
static uint8_t _recv_byte()
{
    uint8_t c = uart_blocking_get(UART_KLINE);

    uart_puts(UART_DEBUG, "RX: ");
    uart_puthex(UART_DEBUG, c);
    uart_put(UART_DEBUG, '\n');
    return c;
}


// Receive byte and send its complement
static uint8_t _recv_byte_send_compl()
{
    uint8_t c = _recv_byte();
    uint8_t complement = c ^ 0xFF;

    _delay_ms(1);

    uart_blocking_put(UART_KLINE, complement);      // send complement byte
    uint8_t echo = uart_blocking_get(UART_KLINE);   // consume its echo
    if (echo != complement) { _panic("complement echo wrong"); }

    uart_puts(UART_DEBUG, "T_: ");
    uart_puthex(UART_DEBUG, complement);
    uart_put(UART_DEBUG, '\n');

    return c;
}


// Wait for the 0x55 0x01 0x8A sequence during initial connection
static void _wait_for_55_01_8a()
{
    uint8_t i = 0;
    uint8_t c = 0;

    while (1) {
        c = _recv_byte();

        if ((i == 0) && (c == 0x55)) { i = 1; }
        if ((i == 1) && (c == 0x01)) { i = 2; }
        if ((i == 2) && (c == 0x8A)) { i = 3; }
        if (i == 3) { break; }
    }
    uart_puts(UART_DEBUG, "\nGOT KW\n\n");
}


// Receive a block
void kwp_receive_block()
{
    uart_puts(UART_DEBUG, "BEGIN RECEIVE BLOCK\n");

    kwp_rx_size = 0;
    memset(kwp_rx_buf, 0, sizeof(kwp_rx_buf));

    uint8_t bytes_remaining = 1;
    uint8_t c;

    while (bytes_remaining) {
        if ((kwp_rx_size == 0) || (bytes_remaining > 1)) {
            c = _recv_byte_send_compl();
        } else {
            // do not send complement for last byte in block (0x03 block end)
            c = _recv_byte();
        }

        kwp_rx_buf[kwp_rx_size++] = c;

        // detect buffer overflow
        if (kwp_rx_size == sizeof(kwp_rx_buf)) { _panic("rx buf overflow"); }

        switch (kwp_rx_size) {
            case 1:  // block length
                bytes_remaining = c;
                break;
            case 2:  // block counter
                if (kwp_is_first_block) {   // set initial value
                    kwp_block_counter = c;
                    kwp_is_first_block = 0;
                } else {                    // increment; detect mismatch
                    if (++kwp_block_counter != c) { _panic("block counter wrong"); }
                }
                // fall through
            default:
                bytes_remaining--;
        }
    }

    _delay_ms(10);
    uart_puts(UART_DEBUG, "END RECEIVE BLOCK\n\n");
    return;
}


// Receive a block; halt unless it has the expected title
void kwp_receive_block_expect(uint8_t title)
{
    kwp_receive_block();
    if (kwp_rx_buf[2] == title) { return; }

    uart_flush_tx(UART_DEBUG);
    uart_puts(UART_DEBUG, "\n\nExpected to receive title 0x");
    uart_puthex(UART_DEBUG, title);
    uart_puts(UART_DEBUG, ", got 0x");
    uart_puthex(UART_DEBUG, kwp_rx_buf[2]);
    uart_put(UART_DEBUG, '\n');
    _panic("rx block title wrong");
}


void kwp_send_ack_block()
{
    uart_puts(UART_DEBUG, "BEGIN SEND BLOCK: ACK\n");

    _send_byte_recv_compl(0x03);                // block length
    _send_byte_recv_compl(++kwp_block_counter); // block counter
    _send_byte_recv_compl(KWP_ACK);             // block title
    _send_byte(0x03);                           // block end

    uart_puts(UART_DEBUG, "END SEND BLOCK: ACK\n\n");
}


void kwp_send_f0_block()
{
    uart_puts(UART_DEBUG, "BEGIN SEND BLOCK: F0\n");

    _send_byte_recv_compl(0x04);                // block length
    _send_byte_recv_compl(++kwp_block_counter); // block counter
    _send_byte_recv_compl(KWP_SAFE_CODE);       // block title
    _send_byte_recv_compl(0x00);                // 0=read
    _send_byte(0x03);                           // block end

    uart_puts(UART_DEBUG, "END SEND BLOCK: F0\n\n");
}


void kwp_send_login_block(uint16_t safe_code, uint8_t fern, uint16_t workshop)
{
    uart_puts(UART_DEBUG, "BEGIN SEND BLOCK: LOGIN\n");

    _send_byte_recv_compl(0x08);                 // block length
    _send_byte_recv_compl(++kwp_block_counter);  // block counter
    _send_byte_recv_compl(KWP_LOGIN);            // block title
    _send_byte_recv_compl(HIGH(safe_code));      // safe code high byte
    _send_byte_recv_compl(LOW(safe_code));       // safe code low byte
    _send_byte_recv_compl(fern);                 // fern byte
    _send_byte_recv_compl(HIGH(workshop));       // workshop code high byte
    _send_byte_recv_compl(LOW(workshop));        // workshop code low byte
    _send_byte(0x03);                            // block end

    uart_puts(UART_DEBUG, "END SEND BLOCK: LOGIN\n\n");
}


void kwp_send_group_reading_block(uint8_t group)
{
    uart_puts(UART_DEBUG, "BEGIN SEND BLOCK: GROUP READ\n");

    _send_byte_recv_compl(0x04);                // block length
    _send_byte_recv_compl(++kwp_block_counter); // block counter
    _send_byte_recv_compl(KWP_GROUP_READING);   // block title
    _send_byte_recv_compl(group);               // group number
    _send_byte(0x03);                           // block end

    uart_puts(UART_DEBUG, "END SEND BLOCK: GROUP READ\n\n");
}


void kwp_send_read_eeprom_block(uint16_t address, uint8_t length)
{
    uart_puts(UART_DEBUG, "BEGIN SEND BLOCK: READ EEPROM\n");

    _send_byte_recv_compl(0x06);                 // block length
    _send_byte_recv_compl(++kwp_block_counter);  // block counter
    _send_byte_recv_compl(KWP_READ_EEPROM);      // block title (read eeprom)
    _send_byte_recv_compl(length);               // number of bytes to read
    _send_byte_recv_compl(HIGH(address));        // address high
    _send_byte_recv_compl(LOW(address));         // address low
    _send_byte(0x03);                            // block end

    uart_puts(UART_DEBUG, "END SEND BLOCK: READ EEPROM\n\n");
}


void kwp_send_read_ram_block(uint16_t address, uint8_t length)
{
    uart_puts(UART_DEBUG, "BEGIN SEND BLOCK: READ RAM\n");

    _send_byte_recv_compl(0x06);                 // block length
    _send_byte_recv_compl(++kwp_block_counter);  // block counter
    _send_byte_recv_compl(KWP_READ_RAM);         // block title
    _send_byte_recv_compl(length);               // number of bytes to read
    _send_byte_recv_compl(HIGH(address));        // address high
    _send_byte_recv_compl(LOW(address));         // address low
    _send_byte(0x03);                            // block end

    uart_puts(UART_DEBUG, "END SEND BLOCK: READ RAM\n\n");
}


uint16_t kwp_read_safe_code_bcd()
{
    kwp_send_f0_block();
    kwp_receive_block_expect(KWP_SAFE_CODE);
    uint16_t safe_code_bcd = (kwp_rx_buf[3] << 8) + kwp_rx_buf[4];
    return safe_code_bcd;
}


void kwp_read_ram(uint16_t start_address, uint16_t size)
{
    uint16_t address = start_address;
    uint16_t remaining = size;

    while (remaining != 0) {
        uint8_t chunksize = 32;
        if (remaining < chunksize) { chunksize = remaining; }

        kwp_send_read_ram_block(address, chunksize);
        kwp_receive_block_expect(KWP_R_READ_RAM);

        uart_puts(UART_DEBUG, "RAM: ");
        uart_puthex16(UART_DEBUG, address);
        uart_puts(UART_DEBUG, ": ");
        for (uint8_t i=0; i<chunksize; i++) {
            uart_puthex(UART_DEBUG, kwp_rx_buf[3 + i]);
            uart_put(UART_DEBUG, ' ');
        }
        uart_puts(UART_DEBUG, "\n\n");

        address += chunksize;
        remaining -= chunksize;

        kwp_send_ack_block(address, chunksize);
        kwp_receive_block_expect(KWP_ACK);
    }
}


void kwp_read_eeprom()
{
    uint16_t address = 0;
    uint16_t remaining = 0x80;

    while (remaining != 0) {
        uint8_t chunksize = 32;
        if (remaining < chunksize) { chunksize = remaining; }

        kwp_send_read_eeprom_block(address, chunksize);
        kwp_receive_block_expect(KWP_R_READ_EEPROM);

        uart_puts(UART_DEBUG, "EEPROM: ");
        uart_puthex16(UART_DEBUG, address);
        uart_puts(UART_DEBUG, ": ");
        for (uint8_t i=0; i<chunksize; i++) {
            uart_puthex(UART_DEBUG, kwp_rx_buf[3 + i]);
            uart_put(UART_DEBUG, ' ');
        }
        uart_puts(UART_DEBUG, "\n\n");

        address += chunksize;
        remaining -= chunksize;

        kwp_send_ack_block(address, chunksize);
        kwp_receive_block_expect(KWP_ACK);
    }
}


void kwp_connect(uint8_t address, uint32_t baud)
{
    uart_init(UART_KLINE, baud);

    _send_address(address);
    _wait_for_55_01_8a();
    _delay_ms(30);
    _send_byte(0x75);

    kwp_is_first_block = 1;

    memset(kwp_vag_number,  0, sizeof(kwp_vag_number));
    memset(kwp_component_1, 0, sizeof(kwp_component_1));
    memset(kwp_component_2, 0, sizeof(kwp_component_2));

    for (uint8_t i=0; i<4; i++) {
        kwp_receive_block_expect(KWP_R_ASCII_DATA);
        kwp_send_ack_block();

        switch (i) {
            case 0:     // 0xF6 (ASCII/Data): "1J0035180D  "
                memcpy(&kwp_vag_number,  &kwp_rx_buf[3], 12);
                break;
            case 1:     // 0xF6 (ASCII/Data): " RADIO 3CP  "
                memcpy(&kwp_component_1, &kwp_rx_buf[3], 12);
                break;
            case 2:     // 0xF6 (ASCII/Data): "        0001"
                memcpy(&kwp_component_2, &kwp_rx_buf[3], 12);
                break;
            default:    // 0xF6 (ASCII/Data): 0x00 0x0A 0xF8 0x00 0x00
                break;
        }
    }

    // Receive 0x09 (Acknowledge)
    kwp_receive_block_expect(KWP_ACK);
}