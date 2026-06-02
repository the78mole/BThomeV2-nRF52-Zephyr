/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Daniel Glaser
 *
 * BThome V2 encoder for Zephyr / nRF Connect SDK
 *
 * Specification: https://bthome.io/format/
 * Reference:     https://github.com/the78mole/bthomev2
 */

#include <bthome_v2/bthome_v2.h>

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------*/

/**
 * @brief Append a single byte to the service-data staging area (context buf).
 *        Returns -ENOMEM if there is no room left.
 */
static int append_byte(uint8_t *buf, uint8_t *pos, uint8_t max_len,
		       uint8_t byte)
{
	if (*pos >= max_len) {
		return -ENOMEM;
	}
	buf[(*pos)++] = byte;
	return 0;
}

/**
 * @brief Try to find a free slot in the measurement queue.
 * @return Pointer to the free slot, or NULL if the queue is full.
 */
static struct bthome_v2_meas *alloc_meas(struct bthome_v2_ctx *ctx)
{
	if (ctx->meas_count >= BTHOME_V2_MAX_MEASUREMENTS) {
		return NULL;
	}
	return &ctx->meas[ctx->meas_count++];
}

/** @brief Add a 1-byte measurement. */
static int add_u8(struct bthome_v2_ctx *ctx, uint8_t obj_id, uint8_t val)
{
	struct bthome_v2_meas *m = alloc_meas(ctx);

	if (!m) {
		return -ENOMEM;
	}
	m->obj_id   = obj_id;
	m->data_len = 1U;
	m->data[0]  = val;
	return 0;
}

/** @brief Add a signed 8-bit measurement. */
static int add_s8(struct bthome_v2_ctx *ctx, uint8_t obj_id, int8_t val) __attribute__((unused));
static int add_s8(struct bthome_v2_ctx *ctx, uint8_t obj_id, int8_t val)
{
	return add_u8(ctx, obj_id, (uint8_t)val);
}

/** @brief Add a 2-byte little-endian measurement. */
static int add_u16(struct bthome_v2_ctx *ctx, uint8_t obj_id, uint16_t val)
{
	struct bthome_v2_meas *m = alloc_meas(ctx);

	if (!m) {
		return -ENOMEM;
	}
	m->obj_id   = obj_id;
	m->data_len = 2U;
	m->data[0]  = (uint8_t)(val & 0xFFU);
	m->data[1]  = (uint8_t)((val >> 8U) & 0xFFU);
	return 0;
}

/** @brief Add a signed 16-bit little-endian measurement. */
static int add_s16(struct bthome_v2_ctx *ctx, uint8_t obj_id, int16_t val)
{
	return add_u16(ctx, obj_id, (uint16_t)val);
}

/** @brief Add a 3-byte (uint24) little-endian measurement. */
static int add_u24(struct bthome_v2_ctx *ctx, uint8_t obj_id, uint32_t val)
{
	struct bthome_v2_meas *m = alloc_meas(ctx);

	if (!m) {
		return -ENOMEM;
	}
	m->obj_id   = obj_id;
	m->data_len = 3U;
	m->data[0]  = (uint8_t)(val & 0xFFU);
	m->data[1]  = (uint8_t)((val >> 8U) & 0xFFU);
	m->data[2]  = (uint8_t)((val >> 16U) & 0xFFU);
	return 0;
}

/** @brief Add a 4-byte little-endian measurement. */
static int add_u32(struct bthome_v2_ctx *ctx, uint8_t obj_id, uint32_t val)
{
	struct bthome_v2_meas *m = alloc_meas(ctx);

	if (!m) {
		return -ENOMEM;
	}
	m->obj_id   = obj_id;
	m->data_len = 4U;
	m->data[0]  = (uint8_t)(val & 0xFFU);
	m->data[1]  = (uint8_t)((val >> 8U) & 0xFFU);
	m->data[2]  = (uint8_t)((val >> 16U) & 0xFFU);
	m->data[3]  = (uint8_t)((val >> 24U) & 0xFFU);
	return 0;
}

/* ---------------------------------------------------------------------------
 * Sorting helper (insertion sort — stable, O(n²), fine for ≤12 entries)
 * -------------------------------------------------------------------------*/
static void sort_measurements(struct bthome_v2_ctx *ctx)
{
	for (uint8_t i = 1U; i < ctx->meas_count; i++) {
		struct bthome_v2_meas key = ctx->meas[i];
		int j = (int)i - 1;

		while (j >= 0 && ctx->meas[j].obj_id > key.obj_id) {
			ctx->meas[j + 1] = ctx->meas[j];
			j--;
		}
		ctx->meas[j + 1] = key;
	}
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------*/

void bthome_v2_init(struct bthome_v2_ctx *ctx, bool encrypted,
		    bool trigger_based)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->encrypted     = encrypted;
	ctx->trigger_based = trigger_based;
}

void bthome_v2_clear(struct bthome_v2_ctx *ctx)
{
	ctx->meas_count   = 0U;
	ctx->svc_data_len = 0U;
}

/* ---------------------------------------------------------------------------
 * Measurement add API
 * -------------------------------------------------------------------------*/

int bthome_v2_add_packet_id(struct bthome_v2_ctx *ctx, uint8_t id)
{
	return add_u8(ctx, BTHOME_OBJ_PACKET_ID, id);
}

int bthome_v2_add_battery(struct bthome_v2_ctx *ctx, uint8_t percent)
{
	return add_u8(ctx, BTHOME_OBJ_BATTERY, percent);
}

int bthome_v2_add_temperature(struct bthome_v2_ctx *ctx, int16_t temp_cdegc)
{
	return add_s16(ctx, BTHOME_OBJ_TEMPERATURE, temp_cdegc);
}

int bthome_v2_add_temperature_01(struct bthome_v2_ctx *ctx,
				 int16_t temp_ddegc)
{
	return add_s16(ctx, BTHOME_OBJ_TEMPERATURE_01, temp_ddegc);
}

int bthome_v2_add_humidity(struct bthome_v2_ctx *ctx, uint16_t humidity_cpct)
{
	return add_u16(ctx, BTHOME_OBJ_HUMIDITY, humidity_cpct);
}

int bthome_v2_add_pressure(struct bthome_v2_ctx *ctx, uint32_t pressure_chpa)
{
	return add_u24(ctx, BTHOME_OBJ_PRESSURE, pressure_chpa);
}

int bthome_v2_add_illuminance(struct bthome_v2_ctx *ctx,
			      uint32_t illuminance_clx)
{
	return add_u24(ctx, BTHOME_OBJ_ILLUMINANCE, illuminance_clx);
}

int bthome_v2_add_co2(struct bthome_v2_ctx *ctx, uint16_t ppm)
{
	return add_u16(ctx, BTHOME_OBJ_CO2, ppm);
}

int bthome_v2_add_tvoc(struct bthome_v2_ctx *ctx, uint16_t ugm3)
{
	return add_u16(ctx, BTHOME_OBJ_TVOC, ugm3);
}

int bthome_v2_add_voltage(struct bthome_v2_ctx *ctx, uint16_t millivolts)
{
	return add_u16(ctx, BTHOME_OBJ_VOLTAGE, millivolts);
}

int bthome_v2_add_dew_point(struct bthome_v2_ctx *ctx, int16_t temp_cdegc)
{
	return add_s16(ctx, BTHOME_OBJ_DEW_POINT, temp_cdegc);
}

int bthome_v2_add_acceleration(struct bthome_v2_ctx *ctx, uint16_t milli_ms2)
{
	return add_u16(ctx, BTHOME_OBJ_ACCELERATION, milli_ms2);
}

int bthome_v2_add_acceleration_axis(struct bthome_v2_ctx *ctx,
				    int32_t micro_ms2)
{
	return add_u32(ctx, BTHOME_OBJ_ACCELERATION_AXIS, (uint32_t)micro_ms2);
}

int bthome_v2_add_gyroscope(struct bthome_v2_ctx *ctx, uint16_t milli_degs)
{
	return add_u16(ctx, BTHOME_OBJ_GYROSCOPE, milli_degs);
}

int bthome_v2_add_timestamp(struct bthome_v2_ctx *ctx, uint32_t unix_s)
{
	return add_u32(ctx, BTHOME_OBJ_TIMESTAMP, unix_s);
}

int bthome_v2_add_binary(struct bthome_v2_ctx *ctx, uint8_t obj_id,
			 bool active)
{
	return add_u8(ctx, obj_id, active ? 1U : 0U);
}

int bthome_v2_add_button(struct bthome_v2_ctx *ctx, uint8_t event)
{
	return add_u8(ctx, BTHOME_OBJ_BUTTON, event);
}

int bthome_v2_add_dimmer(struct bthome_v2_ctx *ctx, uint8_t direction,
			 uint8_t steps)
{
	struct bthome_v2_meas *m = alloc_meas(ctx);

	if (!m) {
		return -ENOMEM;
	}
	m->obj_id   = BTHOME_OBJ_DIMMER;
	m->data_len = 2U;
	m->data[0]  = direction;
	m->data[1]  = steps;
	return 0;
}

int bthome_v2_add_raw(struct bthome_v2_ctx *ctx, uint8_t obj_id,
		      const uint8_t *data, uint8_t data_len)
{
	/* Variable-length objects are stored inline; check total fits in buf. */
	if (data_len > BTHOME_V2_MAX_VALUE_LEN) {
		/* For larger payloads the caller must encode manually; this
		 * helper covers the common short-data case.               */
		return -EINVAL;
	}

	struct bthome_v2_meas *m = alloc_meas(ctx);

	if (!m) {
		return -ENOMEM;
	}
	m->obj_id   = obj_id;
	m->data_len = data_len;
	memcpy(m->data, data, data_len);
	return 0;
}

/* ---------------------------------------------------------------------------
 * Encode
 * -------------------------------------------------------------------------*/

int bthome_v2_encode(struct bthome_v2_ctx *ctx)
{
	uint8_t *buf = ctx->svc_data;
	uint8_t  pos = 0U;

	/* Write UUID (little-endian) */
	if (append_byte(buf, &pos, BTHOME_V2_SVC_DATA_MAX_LEN,
			BTHOME_V2_UUID_LE_B0) < 0) {
		return -ENOMEM;
	}
	if (append_byte(buf, &pos, BTHOME_V2_SVC_DATA_MAX_LEN,
			BTHOME_V2_UUID_LE_B1) < 0) {
		return -ENOMEM;
	}

	/* Device information byte */
	uint8_t dev_info = BTHOME_V2_DEV_INFO_VERSION;

	if (ctx->encrypted) {
		dev_info |= BTHOME_V2_DEV_INFO_ENCRYPT;
	}
	if (ctx->trigger_based) {
		dev_info |= BTHOME_V2_DEV_INFO_TRIGGER;
	}
	if (append_byte(buf, &pos, BTHOME_V2_SVC_DATA_MAX_LEN,
			dev_info) < 0) {
		return -ENOMEM;
	}

	/* Sort measurements by object ID (spec requirement) */
	sort_measurements(ctx);

	/* Serialise measurements */
	for (uint8_t i = 0U; i < ctx->meas_count; i++) {
		const struct bthome_v2_meas *m = &ctx->meas[i];

		if (pos + 1U + m->data_len > BTHOME_V2_SVC_DATA_MAX_LEN) {
			/* Not enough space — stop gracefully */
			break;
		}
		buf[pos++] = m->obj_id;
		memcpy(&buf[pos], m->data, m->data_len);
		pos += m->data_len;
	}

	ctx->svc_data_len = pos;
	return (int)pos;
}

/* ---------------------------------------------------------------------------
 * bt_data helper
 * -------------------------------------------------------------------------*/

int bthome_v2_get_bt_data(const struct bthome_v2_ctx *ctx,
			  struct bt_data *out_data)
{
	if (!ctx || !out_data || ctx->svc_data_len == 0U) {
		return -EINVAL;
	}

	out_data->type     = BT_DATA_SVC_DATA16;
	out_data->data_len = ctx->svc_data_len;
	out_data->data     = ctx->svc_data;
	return 0;
}
