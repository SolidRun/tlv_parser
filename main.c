/* SPDX-License-Identifier: BSD-4.
 * Copyright(c) 2023 SolidRun ltd. All rights reserved.
 * Author: Alvaro Karsz (alvaro.karsz@solid-run.com)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

#define TLV_CODE_PRODUCT_NAME   0x21
#define TLV_CODE_PART_NUMBER    0x22
#define TLV_CODE_SERIAL_NUMBER  0x23
#define TLV_CODE_MAC_BASE       0x24
#define TLV_CODE_MANUF_DATE     0x25
#define TLV_CODE_DEVICE_VERSION 0x26
#define TLV_CODE_PLATFORM_NAME  0x28
#define TLV_CODE_MAC_SIZE       0x2A
#define TLV_CODE_MANUF_NAME     0x2B
#define TLV_CODE_MANUF_COUNTRY  0x2C
#define TLV_CODE_VENDOR_NAME    0x2D
#define TLV_CODE_VENDOR_EXT     0xFD
#define TLV_CODE_CRC_32         0xFE

struct tlv_hdr {
        /* TLV signature */
        char sig[8];
        /* Version */
	uint8_t ver;
        /* Total length */
	uint16_t len;
} __attribute__((packed));

struct tlv_field {
	uint8_t type;
	uint8_t len;
	uint8_t *val;
} __attribute__((packed));

#define TLV_MAX_LEN         256
static uint8_t eeprom[TLV_MAX_LEN];
static uint16_t busnum = 0xFFFF;
static uint16_t eeprom_addr = 0xFFFF;

#define GET_TLV_HDR()                ((struct tlv_hdr *)eeprom)
#define GET_TLV_LEN()                __builtin_bswap16(GET_TLV_HDR()->len)
#define GET_TLV_FIELD(off)           ((struct tlv_field *)((uint64_t)eeprom + (off)))
#define GET_TLV_DATA(off, len, buf)  (memcpy((buf), (void *)((uint64_t)eeprom + (off)), (len)))

static uint32_t tlv_calc_crc32(uint8_t *buf, uint16_t len)
{
        int i, j;
        uint32_t byte, crc, mask;

        i = 0;
        crc = 0xFFFFFFFF;
        while (len--) {
                byte = buf[i];
                crc = crc ^ byte;
                for (j = 7; j >= 0; j--) {
                        mask = -(crc & 1);
                        crc = (crc >> 1) ^ (0xEDB88320 & mask);
                }
                i = i + 1;
        }
        return ~crc;
}

static uint16_t tlv_find(uint8_t code, uint8_t *output, uint16_t *offset_out)
{
        struct tlv_field *field;
        struct tlv_hdr *hdr;
        uint16_t tot_len, offset;

        hdr = GET_TLV_HDR();
        offset = sizeof(struct tlv_hdr);
        tot_len = GET_TLV_LEN() + offset;

        /* Search for code */
        for (;;) {
                /* Get next field, read only the code and length */
                field = GET_TLV_FIELD(offset);

                /* Is this the code we are looking for? if so, load data and return status */
                if (field->type == code) {
                        if (output)
                                GET_TLV_DATA(offset + 2, field->len, output);
                        if (offset_out)
                                *offset_out = offset;

                        return field->len;
                }

                /* Move offset */
                offset += 2 + field->len;

                /* Have we reached to the last value? (CRC)
                 * If so, the value was not found..
                 */
                if (field->type == TLV_CODE_CRC_32)
                        return 0;

                /* Sanity check - data not found*/
                if (offset >= tot_len)
                        return 0;
        }
}

static bool tlv_verify_crc32(void)
{
        uint32_t crc_calc, crc_val;

        crc_calc = tlv_calc_crc32(eeprom, GET_TLV_LEN() + sizeof(struct tlv_hdr) - 4);//Don't CRC the CRC itself :)

        /* Now find the CRC value in the EEPROM */
        if (!tlv_find(TLV_CODE_CRC_32, (uint8_t *)&crc_val, NULL)) {
                printf("No CRC32 value found in TLV..\n");
                return false;
        }

        return __builtin_bswap32(crc_val) == crc_calc;
}

bool i2c_read(int fd, uint16_t addr, uint8_t reg, uint16_t len, void *output)
{
        struct i2c_rdwr_ioctl_data i2c_data;
        struct i2c_msg i2c_msgs[2];

        i2c_msgs[0].addr = addr;
        i2c_msgs[0].flags = 0;
        i2c_msgs[0].len = sizeof(reg);
        i2c_msgs[0].buf = &reg;

        i2c_msgs[1].addr = addr;
        i2c_msgs[1].flags = I2C_M_RD;
        i2c_msgs[1].len = len;
        i2c_msgs[1].buf = (uint8_t *)output;

        i2c_data.msgs = i2c_msgs;
        i2c_data.nmsgs = 2;

        errno = 0;
        if (ioctl(fd, I2C_RDWR, &i2c_data) < 0) {
                printf("ioctl error while reading data from I2C bus - %s\n", strerror(errno));
                return false;
        }

        return true;
}

static void i2c_close_bus(int fd)
{
        close(fd);
}

static int i2c_open_bus(uint16_t busnum)
{
        int fd;
        char filename[30];

        snprintf(filename, 30, "/dev/i2c-%u", busnum);

        errno = 0;
        fd = open(filename, O_RDWR);
        if (fd == -1)
                printf("Could not open I2C device - %s\n", strerror(errno));

        return fd;
}

static bool is_valid_tlvhdr(struct tlv_hdr *hdr)
{
        return strcmp(hdr->sig, "TlvInfo") == 0 &&
                      __builtin_bswap16(hdr->len) <= (TLV_MAX_LEN - sizeof(struct tlv_hdr));
}

static bool read_eeprom(uint16_t busnum, uint16_t addr)
{
        struct tlv_hdr *hdr;
        uint8_t *buf_ptr = eeprom;
        int fd;
        bool status = true;

        fd = i2c_open_bus(busnum);
        if (fd == -1) {
                printf("Can't read EEPROM..\n");
                return false;
        }

        /* Load tlv header */
        status = i2c_read(fd, addr, 0, sizeof(struct tlv_hdr),
                          buf_ptr);
        if (!status) {
                printf("Can't read EEPROM..\n");
                goto close;
        }

        buf_ptr += sizeof(struct tlv_hdr);
        hdr = GET_TLV_HDR();

        /* Validate TLV header */
        if (!is_valid_tlvhdr(hdr)) {
                status = false;
                printf("Invalid TLV header read from EEPROM!\n");
                goto close;
        }


        /* Read entire TLV data */
        status = i2c_read(fd, addr, sizeof(struct tlv_hdr),
                          GET_TLV_LEN(), buf_ptr);
        if (!status) {
                printf("Can't read EEPROM..\n");
                goto close;
        }

        /* Verify TLV CRC */
        if (!tlv_verify_crc32()) {
                status = false;
                printf("Invalid TLV-CRC32 value in EEPROM!\n");
                goto close;
        }

close:
        i2c_close_bus(fd);
        return status;
}

static void print_type_name(uint8_t type)
{
        switch (type) {
        case TLV_CODE_MAC_BASE:
                printf("\tMAC Address base: ");
                break;
        case TLV_CODE_PRODUCT_NAME:
                printf("\tProduct Name: ");
                break;
        case TLV_CODE_PART_NUMBER:
                printf("\tPart Number: ");
                break;
        case TLV_CODE_SERIAL_NUMBER:
                printf("\tSerial Number: ");
                break;
        case TLV_CODE_MANUF_DATE:
                printf("\tManufacturing Date: ");
                break;
        case TLV_CODE_DEVICE_VERSION:
                printf("\tDevice Version: ");
                break;
        case TLV_CODE_PLATFORM_NAME:
                printf("\tPlatform Name: ");
                break;
        case TLV_CODE_MAC_SIZE:
                printf("\tMAC Size: ");
                break;
        case TLV_CODE_MANUF_NAME:
                printf("\tManufacturer Name: ");
                break;
        case TLV_CODE_MANUF_COUNTRY:
                printf("\tManufacturer Country: ");
                break;
        case TLV_CODE_VENDOR_NAME:
                printf("\tVendor Name: ");
                break;
        case TLV_CODE_VENDOR_EXT:
                printf("\tVendor Extension: ");
                break;
        case TLV_CODE_CRC_32:
                printf("\tCRC32: ");
                break;
        default:
                printf("\tUnknown type (%02x): ", type);
                break;
        }
}

static void print_general(struct tlv_field *field)
{
        uint8_t i, *ptr = (uint8_t *)&field->val;

        for ( i = 0; i < field->len; i++) {
                printf("%02x", *ptr++);
                if ( i != 5)
                        printf(" ");
        }
        printf("\n");
}

static void print_string(struct tlv_field *field)
{
        uint8_t i, *ptr = (uint8_t *)&field->val;

        for (i = 0; i < field->len; i++)
                printf("%c", *ptr++);

        printf("\n");

}

static void print_mac(struct tlv_field *field)
{
        uint8_t i, *ptr = (uint8_t *)&field->val;

        if (field->len != 6) {
                printf("Invalid MAC address, expected %u bytes, received %u\n", 6, field->len);
                return;
        }

        for ( i = 0; i < 6; i++) {
                printf("%02x", *ptr++);
                if ( i != 5)
                        printf(":");
        }

        printf("\n");
}

static void print_field(struct tlv_field *field)
{
        print_type_name(field->type);

        switch (field->type) {

        case TLV_CODE_MAC_BASE:
                print_mac(field);
                break;

        case TLV_CODE_PRODUCT_NAME:
        case TLV_CODE_SERIAL_NUMBER:
        case TLV_CODE_PLATFORM_NAME:
        case TLV_CODE_MANUF_COUNTRY:
        case TLV_CODE_VENDOR_NAME:
        case TLV_CODE_MANUF_NAME:
        case TLV_CODE_PART_NUMBER:
                print_string(field);
                break;

        default:
                print_general(field);
                break;

        }
}

static void print_eeprom(void)
{
        struct tlv_field *field;
        struct tlv_hdr *hdr;
        uint16_t tot_len, offset;

        printf("EEPROM Contents:\n");

        hdr = GET_TLV_HDR();
        offset = sizeof(struct tlv_hdr);
        tot_len = GET_TLV_LEN() + offset;

        for (;;) {
                /* Get next field, read only the code and length */
                field = GET_TLV_FIELD(offset);
                print_field(field);
                /* Move offset */
                offset += 2 + field->len;

                if (offset >= tot_len)
                        break;
        }

        printf("\n");
}

static void parse_args(int argc, char **argv)
{
        int opt;

        while ((opt = getopt(argc, argv, "b:a:")) != -1) {
                switch (opt) {
                case 'b':
                        busnum = strtol(optarg, NULL, 0);
                        break;
                case 'a':
                        eeprom_addr = strtol(optarg, NULL, 0);
                        break;
                default:
                        printf("Unknown argument %c\n", opt);
                        break;
                }
        }
}

static void print_usage(char *program)
{
        printf("Usage:\n\n%s -b <I2C bus number> -a <EEPROM address>\n\n", program);
}

int main(int argc, char **argv)
{
        int ret = -1;

        parse_args(argc, argv);

        if (busnum == 0xFFFF || eeprom_addr == 0xFFFF) {
                print_usage(argv[0]);
                goto exit;
        }

        /* Load EEPROM content into RAM */
        if (!read_eeprom(busnum, eeprom_addr))
                goto exit;

        /* Print EEPROM data */
        print_eeprom();

        ret = 0;
exit:
        return ret;
}
