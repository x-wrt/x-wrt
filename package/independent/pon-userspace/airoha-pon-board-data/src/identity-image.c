// SPDX-License-Identifier: GPL-2.0-only

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RI_IMAGE_SIZE      0x40000
#define RI_RECORD_SIZE     0x80
#define RI_CHECKSUM_OFFSET 0x7c

static int hex_value(char value)
{
	value = toupper((unsigned char)value);
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}

static int parse_number(const char *text, size_t *value)
{
	char *end;
	unsigned long number;

	errno = 0;
	number = strtoul(text, &end, 10);
	if (errno || !*text || *end)
		return -1;
	*value = number;
	return 0;
}

static int valid_mac(const uint8_t mac[6])
{
	size_t i;

	if (mac[0] & 1)
		return 0;
	for (i = 0; i < 6 && mac[i] == 0; i++)
		;
	return i != 6;
}

static int parse_mac(const char *text, uint8_t mac[6])
{
	char digits[12];
	size_t count = 0;
	size_t i;

	for (; *text; text++) {
		if (*text == ':' || *text == '-')
			continue;
		if (count == sizeof(digits) || hex_value(*text) < 0)
			return -1;
		digits[count++] = *text;
	}
	if (count != sizeof(digits))
		return -1;
	for (i = 0; i < 6; i++)
		mac[i] = (hex_value(digits[i * 2]) << 4) | hex_value(digits[i * 2 + 1]);
	return valid_mac(mac) ? 0 : -1;
}

static int parse_pon_sn(const char *text, uint8_t serial[12], uint8_t vssn[4])
{
	size_t i;

	if (strlen(text) != 12)
		return -1;
	for (i = 0; i < 4; i++) {
		if (!isalnum((unsigned char)text[i]))
			return -1;
		serial[i] = toupper((unsigned char)text[i]);
	}
	for (i = 4; i < 12; i++) {
		if (hex_value(text[i]) < 0)
			return -1;
		serial[i] = toupper((unsigned char)text[i]);
	}
	for (i = 0; i < 4; i++)
		vssn[i] = (hex_value(text[4 + i * 2]) << 4) | hex_value(text[5 + i * 2]);
	return 0;
}

static uint8_t *load_image(const char *path, size_t *size)
{
	FILE *file;
	uint8_t *image;
	long length;

	file = fopen(path, "rb");
	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END))
		goto fail;
	length = ftell(file);
	if (length <= 0)
		goto fail;
	rewind(file);
	image = malloc(length);
	if (!image)
		goto fail;
	if (fread(image, 1, length, file) != (size_t)length) {
		free(image);
		goto fail;
	}
	fclose(file);
	*size = length;
	return image;

fail:
	fclose(file);
	return NULL;
}

static int save_image(const char *path, const uint8_t *image, size_t size)
{
	FILE *file;
	int result;

	file = fopen(path, "wb");
	if (!file)
		return -1;
	result = fwrite(image, 1, size, file) == size ? 0 : -1;
	if (fclose(file))
		result = -1;
	return result;
}

static void print_mac(const uint8_t mac[6])
{
	printf("%02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int read_field(const uint8_t *field, size_t size, const char *encoding)
{
	uint8_t mac[6], serial[12], vssn[4];
	char text[256];
	size_t i;

	if (!strcmp(encoding, "mac-binary6") && size == 6) {
		if (valid_mac(field))
			print_mac(field);
		return 0;
	}
	if ((!strcmp(encoding, "mac-hex12") && size == 12) ||
	    (!strcmp(encoding, "mac-colon17") && size == 17)) {
		memcpy(text, field, size);
		text[size] = 0;
		if (!parse_mac(text, mac))
			print_mac(mac);
		return 0;
	}
	if (!strcmp(encoding, "pon-ascii12") && size == 12) {
		memcpy(text, field, size);
		text[size] = 0;
		if (!parse_pon_sn(text, serial, vssn))
			printf("%.12s\n", (char *)serial);
		return 0;
	}
	if (!strcmp(encoding, "pon-binary8") && size == 8) {
		for (i = 0; i < 4; i++)
			if (!isalnum(field[i]))
				return 0;
		printf("%c%c%c%c%02X%02X%02X%02X\n", field[0], field[1], field[2], field[3],
		       field[4], field[5], field[6], field[7]);
		return 0;
	}
	if (!strcmp(encoding, "serial-text") && size < sizeof(text)) {
		for (i = 0; i < size && field[i] && field[i] != 0xff; i++)
			if (!isprint(field[i]))
				return 0;
		printf("%.*s\n", (int)i, field);
		return 0;
	}
	return -1;
}

static int write_field(uint8_t *field, size_t size, const char *encoding, const char *value,
                       uint8_t vssn[4])
{
	uint8_t mac[6], serial[12];
	size_t i;

	if (!strncmp(encoding, "mac-", 4)) {
		if (parse_mac(value, mac))
			return -1;
		if (!strcmp(encoding, "mac-binary6") && size == 6) {
			memcpy(field, mac, 6);
			return 0;
		}
		if (!strcmp(encoding, "mac-hex12") && size == 12) {
			char text[13];

			for (i = 0; i < 6; i++)
				sprintf(text + i * 2, "%02X", mac[i]);
			memcpy(field, text, size);
			return 0;
		}
		if (!strcmp(encoding, "mac-colon17") && size == 17) {
			char text[18];

			snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
			         mac[1], mac[2], mac[3], mac[4], mac[5]);
			memcpy(field, text, size);
			return 0;
		}
		return -1;
	}
	if (!strncmp(encoding, "pon-", 4)) {
		if (parse_pon_sn(value, serial, vssn))
			return -1;
		if (!strcmp(encoding, "pon-ascii12") && size == 12) {
			memcpy(field, serial, 12);
			return 0;
		}
		if (!strcmp(encoding, "pon-binary8") && size == 8) {
			memcpy(field, serial, 4);
			memcpy(field + 4, vssn, 4);
			return 0;
		}
		return -1;
	}
	if (!strcmp(encoding, "serial-text") && strlen(value) <= size) {
		for (i = 0; value[i]; i++)
			if (!isprint((unsigned char)value[i]))
				return -1;
		memset(field, 0, size);
		memcpy(field, value, i);
		return 0;
	}
	return -1;
}

/* Each RI record stores a 16-bit two's-complement checksum of its data bytes. */
static void update_ri_checksum(uint8_t *record)
{
	uint32_t sum = 0;
	uint16_t checksum;
	size_t i;

	for (i = 0; i < RI_CHECKSUM_OFFSET; i++)
		sum += record[i];
	checksum = (uint16_t)(0u - sum);
	record[RI_CHECKSUM_OFFSET] = checksum;
	record[RI_CHECKSUM_OFFSET + 1] = checksum >> 8;
}

int main(int argc, char **argv)
{
	const char *encoding, *codec = "-";
	uint8_t *image, vssn[4] = {0};
	size_t image_size, offset, size, mirror = 0, i;
	int writing, status;

	writing = argc == 10 && !strcmp(argv[1], "write");
	if (!(argc == 6 && !strcmp(argv[1], "read")) && !writing)
		return 2;
	image = load_image(argv[2], &image_size);
	if (!image) {
		fprintf(stderr, "Cannot read identity image\n");
		return 1;
	}
	if (parse_number(argv[writing ? 4 : 3], &offset) ||
	    parse_number(argv[writing ? 5 : 4], &size) || !size || offset > image_size ||
	    size > image_size - offset) {
		fprintf(stderr, "Identity field exceeds image bounds\n");
		free(image);
		return 2;
	}
	encoding = argv[writing ? 6 : 5];
	if (!writing) {
		status = read_field(image + offset, size, encoding);
		free(image);
		return status ? 2 : 0;
	}
	codec = argv[7];
	if (strcmp(codec, "-") && (strcmp(codec, "nokia-ri-v1") || image_size != RI_IMAGE_SIZE)) {
		free(image);
		return 2;
	}
	if (strcmp(argv[8], "-") &&
	    (image_size < 4 || parse_number(argv[8], &mirror) || mirror > image_size - 4)) {
		free(image);
		return 2;
	}

	/* RI checksums cover two records initialized with ASCII zeroes. */
	if (!strcmp(codec, "nokia-ri-v1")) {
		for (i = 0; i < image_size && image[i] == 0xff; i++)
			;
		if (i == image_size)
			memset(image, '0', 0x200);
	}
	if (write_field(image + offset, size, encoding, argv[9], vssn)) {
		fprintf(stderr, "Invalid identity value\n");
		free(image);
		return 2;
	}
	if (strcmp(argv[8], "-")) {
		if (strcmp(codec, "nokia-ri-v1") || strcmp(encoding, "pon-ascii12")) {
			free(image);
			return 2;
		}
		memcpy(image + mirror, vssn, 4);
	}
	if (!strcmp(codec, "nokia-ri-v1")) {
		update_ri_checksum(image);
		update_ri_checksum(image + RI_RECORD_SIZE);
	}
	status = save_image(argv[3], image, image_size);
	free(image);
	return status ? 1 : 0;
}
