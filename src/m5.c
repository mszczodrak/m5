/*
 *                                   1byt3
 *
 *                              License Notice
 *
 * 1byt3 provides a commercial license agreement for this software. This
 * commercial license can be used for development of proprietary/commercial
 * software. Under this commercial license you do not need to comply with the
 * terms of the GNU Affero General Public License, either version 3 of the
 * License, or (at your option) any later version.
 *
 * If you don't receive a commercial license from us (1byt3), you MUST assume
 * that this software is distributed under the GNU Affero General Public
 * License, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Contact us for additional information: customers at 1byt3.com
 *
 *                          End of License Notice
 */

/*
 * m5: MQTT 5 Low Level Packet Library
 *
 * Copyright (C) 2017 1byt3, customers at 1byt3.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "m5.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define APPBUF_FREE_READ_SPACE(buf) (buf->len - buf->offset)
#define APPBUF_FREE_WRITE_SPACE(buf) (buf->size - buf->len)
#define APPBUF_DATAPTR_CURRENT(buf) (buf->data + buf->offset)

#define M5_BINARY_LEN_SIZE	2u
#define M5_STR_LEN_SIZE		M5_BINARY_LEN_SIZE
#define M5_INT_LEN_SIZE		2
#define M5_PROTO_STR		"MQTT"
#define M5_PROTO_NAME_LEN	6u
#define M5_PROTO_VERSION5	0x05

#define M5_CLIENTID_MIN_LEN	1
#define M5_CLIENTID_MAX_LEN	23

#define M5_PACKET_TYPE_WSIZE	1

static int m5_rlen_wsize(uint32_t val, uint32_t *wsize)
{
	if (val > 268435455) {
		return -EINVAL;
	}

	if (val <= 127) {
		*wsize = 1;
	} else if (val <= 16383) {
		*wsize = 2;
	} else if (val <= 2097151) {
		*wsize = 3;
	} else if (val <= 268435455) {
		*wsize = 4;
	}

	return EXIT_SUCCESS;
}

static int m5_encode_int(struct app_buf *buf, uint32_t value)
{
	do {
		uint8_t encoded;

		if (buf->len >= buf->size) {
			return -ENOMEM;
		}

		encoded = value % 128;
		value = value / 128;
		if (value > 0) {
			encoded = encoded | 128;
		}

		buf->data[buf->len] = encoded;
		buf->len += 1;
	} while (value > 0);

	return EXIT_SUCCESS;
}

static int m5_decode_int(struct app_buf *buf, uint32_t *value,
			 uint32_t *val_wsize)
{
	uint32_t multiplier = 1;
	uint8_t encoded;
	int i = 0;

	*value = 0;
	do {
		if (APPBUF_FREE_READ_SPACE(buf) < 1) {
			return -ENOMEM;
		}

		if (multiplier > 128 * 128 * 128) {
			return -EINVAL;
		}

		encoded = buf->data[buf->offset + i++];

		*value += (encoded & 127) * multiplier;
		multiplier *= 128;
	} while ((encoded & 128) != 0);


	buf->offset += i;
	*val_wsize = i;

	return EXIT_SUCCESS;
}

static void m5_add_u8(struct app_buf *buf, uint8_t val)
{
	buf->data[buf->len] = val;
	buf->len += 1;
}

static void m5_add_u16(struct app_buf *buf, uint16_t val)
{
	uint16_t net_order = htobe16(val);
	uint8_t *p = (uint8_t *)&net_order;

	buf->data[buf->len + 0] = p[0];
	buf->data[buf->len + 1] = p[1];
	buf->len += 2;
}

static void m5_add_raw_binary(struct app_buf *buf,
			      uint8_t *src, uint16_t src_len)
{
	memcpy(buf->data + buf->len, src, src_len);
	buf->len += src_len;
}

static void m5_add_binary(struct app_buf *buf, uint8_t *src, uint16_t src_len)
{
	m5_add_u16(buf, src_len);
	m5_add_raw_binary(buf, src, src_len);
}

static void m5_str_add(struct app_buf *buf, const char *str)
{
	m5_add_binary(buf, (uint8_t *)str, strlen(str));
}

static int m5_connect_payload_wsize(struct m5_connect *msg,
				    uint32_t *wire_size)
{
	if (msg->client_id_len < M5_CLIENTID_MIN_LEN ||
	    msg->client_id_len > M5_CLIENTID_MAX_LEN) {
		return -EINVAL;
	}

	*wire_size = M5_STR_LEN_SIZE  + msg->client_id_len;

	if (msg->will_msg_len > 0 && msg->will_topic_len > 0) {
		*wire_size += M5_STR_LEN_SIZE  + msg->will_topic_len +
			      M5_BINARY_LEN_SIZE + msg->will_msg_len;
	} else if (msg->will_msg_len  + msg->will_topic_len != 0) {
		return -EINVAL;
	}

	if (msg->user_name_len > 0) {
		*wire_size += M5_STR_LEN_SIZE + msg->user_name_len;
	}

	if (msg->password_len > 0) {
		*wire_size += M5_BINARY_LEN_SIZE + msg->password_len;
	}

	return EXIT_SUCCESS;
}

static void m5_connect_compute_flags(struct m5_connect *msg, uint8_t *flags)
{
	*flags = (msg->clean_start << 0x01) +
		 (msg->will_msg_len > 0 ? (0x01 << 2) : 0) +
		 ((msg->will_qos & 0x03) << 3) +
		 (msg->will_retain == 1 ? (1 << 5) : 0) +
		 (msg->password_len > 0 ? (0x01 << 6) : 0) +
		 (msg->user_name_len > 0 ? (0x01 << 7) : 0);
}

static int m5_pack_connect_payload(struct app_buf *buf, struct m5_connect *msg)
{
	m5_add_binary(buf, msg->client_id, msg->client_id_len);

	if (msg->will_msg_len > 0) {
		m5_add_binary(buf, msg->will_topic, msg->will_topic_len);
		m5_add_binary(buf, msg->will_msg, msg->will_msg_len);
	}

	if (msg->user_name_len > 0) {
		m5_add_binary(buf, msg->user_name, msg->user_name_len);
	}

	if (msg->password_len > 0) {
		m5_add_binary(buf, msg->password, msg->password_len);
	}

	return EXIT_SUCCESS;
}

int m5_pack_connect(struct app_buf *buf, struct m5_connect *msg)
{
	uint32_t payload_wsize;
	uint32_t full_msg_size;
	uint32_t prop_wsize;
	uint32_t rlen_wsize;
	uint32_t rlen;
	uint8_t flags;
	int rc;

	if (buf == NULL || msg == NULL) {
		return -EINVAL;
	}

	/* xxx Assume that there are no properties... */
	prop_wsize = 0;

	rc = m5_connect_payload_wsize(msg, &payload_wsize);
	if (rc != EXIT_SUCCESS) {
		return rc;
	}

	rlen = M5_PROTO_NAME_LEN + 1 + 1 + 2 +
	       1 + prop_wsize + payload_wsize;

	rc = m5_rlen_wsize(rlen, &rlen_wsize);
	if (rc != EXIT_SUCCESS) {
		return rc;
	}

	full_msg_size = M5_PACKET_TYPE_WSIZE + rlen + rlen_wsize;
	if (APPBUF_FREE_WRITE_SPACE(buf) < full_msg_size) {
		return -ENOMEM;
	}

	m5_connect_compute_flags(msg, &flags);

	m5_add_u8(buf, M5_PKT_CONNECT << 4);
	m5_encode_int(buf, rlen);
	m5_str_add(buf, M5_PROTO_STR);
	m5_add_u8(buf, M5_PROTO_VERSION5);
	m5_add_u8(buf, flags);
	m5_add_u16(buf, msg->keep_alive);

	/* xxx Pack properties: 0 length */
	m5_encode_int(buf, prop_wsize);

	rc = m5_pack_connect_payload(buf, msg);

	return rc;
}
