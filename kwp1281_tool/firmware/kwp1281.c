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
static int _wait_for_55_01_8a()
{
    const uint8_t expected_rx_bytes[] = { 0x55, 0x01, 0x8a };
    uint8_t i = 0;
    uint8_t c = 0;
    uint16_t millis = 0;

    while (1) {
        if (uart_rx_ready(UART_KLINE)) {
            c = _recv_byte();
            if (c == expected_rx_bytes[i]) {
                if (++i == 3) { return KWP_SUCCESS; }
            } else {
                i = 0;
            }
        } else {
            _delay_ms(1);
            if (++millis > 3000) { return KWP_TIMEOUT; }
        }
    }
}


// Send a block
void kwp_send_block(uint8_t *buf)
{
    uint8_t block_length = buf[0];
    uint8_t buf_size = block_length + 1;
    uint8_t block_title = buf[2];

    buf[1] = ++kwp_block_counter;   // insert block counter
    buf[buf_size - 1] = 0x03;       // insert block end

    uart_puts(UART_DEBUG, "BEGIN SEND BLOCK ");
    uart_puthex(UART_DEBUG, block_title);
    uart_puts(UART_DEBUG, "\n");

    uint8_t i;
    for (i=0; i<(buf_size-1); i++) {
        _send_byte_recv_compl(buf[i]);
    }
    _send_byte(buf[i]); // block end

    uart_puts(UART_DEBUG, "END SEND BLOCK ");
    uart_puthex(UART_DEBUG, block_title);
    uart_puts(UART_DEBUG, "\n\n");
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
    uart_puts(UART_DEBUG, "SENDING ACK\n");
    uint8_t block[] = {
        0x03,       // block length
        0,          // placeholder for block counter
        KWP_ACK,    // block title
        0,          // placeholder for block end
    };
    kwp_send_block(block);
}


void kwp_send_login_block(uint16_t safe_code, uint8_t fern, uint16_t workshop)
{
    uart_puts(UART_DEBUG, "SENDING LOGIN\n");
    uint8_t block[] = {
        0x08,               // block length
        0,                  // placeholder for block counter
        KWP_LOGIN,          // block title
        HIGH(safe_code),    // safe code high byte
        LOW(safe_code),     // safe code low byte
        fern,               // fern byte
        HIGH(workshop),     // workshop code high byte
        LOW(workshop),      // workshop code low byte
        0,                  // placeholder for block end
    };
    kwp_send_block(block);
}


void kwp_send_group_reading_block(uint8_t group)
{
    uart_puts(UART_DEBUG, "SENDING GROUP READ\n");
    uint8_t block[] = {
        0x04,               // block length
        0,                  // placeholder for block counter
        KWP_GROUP_READING,  // block title
        group,              // group number
        0,                  // placeholder for block end
    };
    kwp_send_block(block);
}


static void _send_read_mem_block(uint8_t title, uint16_t address, uint8_t length)
{
    uart_puts(UART_DEBUG, "SENDING READ xx MEMORY\n");
    uint8_t block[] = {
        0x06,           // block length
        0,              // placeholder for block counter
        title,          // block title
        length,         // number of bytes to read
        HIGH(address),  // address high
        LOW(address),   // address low
        0,              // placeholder for block end
    };
    kwp_send_block(block);
}


static void _read_mem(uint8_t req_title, uint8_t resp_title,
               uint16_t start_address, uint16_t size)
{
    uint16_t address = start_address;
    uint16_t remaining = size;

    while (remaining != 0) {
        uint8_t chunksize = 32;
        if (remaining < chunksize) { chunksize = remaining; }

        _send_read_mem_block(req_title, address, chunksize);
        kwp_receive_block_expect(resp_title);

        uart_puts(UART_DEBUG, "MEM: ");
        uart_puthex16(UART_DEBUG, address);
        uart_puts(UART_DEBUG, ": ");
        for (uint8_t i=0; i<chunksize; i++) {
            uart_puthex(UART_DEBUG, kwp_rx_buf[3 + i]);
            uart_put(UART_DEBUG, ' ');
        }
        uart_puts(UART_DEBUG, "\n\n");

        address += chunksize;
        remaining -= chunksize;

        kwp_send_ack_block();
        kwp_receive_block_expect(KWP_ACK);
    }
}

void kwp_read_ram(uint16_t start_address, uint16_t size)
{
    _read_mem(KWP_READ_RAM, KWP_R_READ_RAM, start_address, size);
}

void kwp_read_rom_or_eeprom(uint16_t start_address, uint16_t size)
{
    _read_mem(KWP_READ_ROM_EEPROM, KWP_R_READ_ROM_EEPROM, start_address, size);
}

void kwp_read_eeprom(uint16_t start_address, uint16_t size)
{
    _read_mem(KWP_READ_EEPROM, KWP_R_READ_ROM_EEPROM, start_address, size);
}

// Premium 4 only ===========================================================

static void _send_f0_block()
{
    uart_puts(UART_DEBUG, "SENDING TITLE F0\n");
    uint8_t block[] = {
        0x04,           // block length
        0,              // placeholder for block length
        KWP_SAFE_CODE,  // block title
        0,              // 0=read
        0,              // placeholder for block end
    };
    kwp_send_block(block);
}

uint16_t kwp_p4_read_safe_code_bcd()
{
    _send_f0_block();
    kwp_receive_block_expect(KWP_SAFE_CODE);
    uint16_t safe_code_bcd = (kwp_rx_buf[3] << 8) + kwp_rx_buf[4];
    return safe_code_bcd;
}

// Premium 5 mfg mode (address 0x7c) only ===================================

uint16_t kwp_p5_read_safe_code_bcd()
{
    _send_read_mem_block(KWP_READ_ROM_EEPROM, 0x0014, 0x02);
    kwp_receive_block_expect(KWP_R_READ_ROM_EEPROM);
    uint16_t safe_code_bin = (kwp_rx_buf[3] << 8) + kwp_rx_buf[4];
    return safe_code_bin;
}

static void _send_calc_rom_checksum_block()
{
    uart_puts(UART_DEBUG, "SENDING ROM CHECKSUM\n");
    uint8_t block[] = {
        0x05,       // block length
        0,          // placeholder for block counter
        KWP_CUSTOM, // block title
        0x31,       // unknown constant
        0x32,       // subtitle (rom checksum)
        0,          // placeholder for block end
    };
    kwp_send_block(block);
}

uint16_t kwp_p5_calc_rom_checksum()
{
    _send_calc_rom_checksum_block();
    kwp_receive_block_expect(0x1B);
    uint16_t checksum = (kwp_rx_buf[5] << 8) + kwp_rx_buf[6];
    return checksum;
}

// ==========================================================================

int kwp_connect(uint8_t address, uint32_t baud)
{
    uart_init(UART_KLINE, baud);

    kwp_is_first_block = 1;

    memset(kwp_vag_number,  0, sizeof(kwp_vag_number));
    memset(kwp_component_1, 0, sizeof(kwp_component_1));
    memset(kwp_component_2, 0, sizeof(kwp_component_2));

    _send_address(address);
    int result = _wait_for_55_01_8a();
    if (result != KWP_SUCCESS) { return result; }  // error
    _delay_ms(30);
    _send_byte(0x75);

    for (uint8_t i=0; i<4; i++) {
        kwp_receive_block();

        // Premium 5 mfg mode (address 0x7C) sends ACK and is ready immediately receiving 0x75
        if ((i == 0) && (kwp_rx_buf[2] == KWP_ACK)) { return KWP_SUCCESS; }

        // All others should send 4 ASCII blocks that need to be ACKed after receiving 0x75
        if (kwp_rx_buf[2] != KWP_R_ASCII_DATA) { _panic("Expected 0xF6"); }

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

        kwp_send_ack_block();
    }

    kwp_receive_block_expect(KWP_ACK);
    return KWP_SUCCESS;
}

int kwp_autoconnect(uint8_t address)
{
    uint16_t bauds[2] = { 10400, 9600 };
    for (uint8_t try=0; try<2; try++) {
        for (uint8_t baud_index=0; baud_index<2; baud_index++) {
            int result = kwp_connect(address, bauds[baud_index]);
            if (result == KWP_SUCCESS) { return result; }
            _delay_ms(2000); // delay before next try
        }
    }
    return KWP_TIMEOUT;
}

void kwp_print_module_info()
{
    uart_puts(UART_DEBUG, "VAG Number: \"");
    for (uint8_t i=0; i<12; i++) {
        uart_put(UART_DEBUG, kwp_vag_number[i]);
    }
    uart_puts(UART_DEBUG, "\"\n");

    uart_puts(UART_DEBUG, "Component:  \"");
    for (uint8_t i=0; i<12; i++) {
        uart_put(UART_DEBUG, kwp_component_1[i]);
    }
    for (uint8_t i=0; i<12; i++) {
        uart_put(UART_DEBUG, kwp_component_2[i]);
    }
    uart_puts(UART_DEBUG, "\"\n");
}
