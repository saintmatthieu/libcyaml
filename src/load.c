/*
 * SPDX-License-Identifier: ISC
 *
 * Copyright (C) 2017 Michael Drake <tlsa@netsurf-browser.org>
 */

/**
 * \file
 * \brief Load YAML data into client's data structure.
 *
 * This uses `libyaml` to parse YAML documents, it validates the documents
 * against the client provided schema, and uses the schema to place the data
 * in the client's data structure.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <strings.h>
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <yaml.h>

#include "data.h"
#include "util.h"

/** Identifies that no mapping schema entry was found for key. */
#define CYAML_SCHEMA_IDX_NONE 0xffff

/** CYAML load state machine states. */
enum cyaml_state_e {
	CYAML_STATE_START,       /**< Initial state. */
	CYAML_STATE_IN_STREAM,   /**< In a stream. */
	CYAML_STATE_IN_DOC,      /**< In a document. */
	CYAML_STATE_IN_MAPPING,  /**< In a mapping. */
	CYAML_STATE_IN_SEQUENCE, /**< In a sequence. */
	CYAML_STATE__COUNT,      /**< Count of states, **not a valid
	                              state itself**. */
};

/** Mapping load state machine sub-states. */
enum cyaml_mapping_state_e {
	/** In state \ref CYAML_STATE_IN_MAPPING expecting **key**. */
	CYAML_MAPPING_STATE_KEY,

	/** In state \ref CYAML_STATE_IN_MAPPING expecting **value**. */
	CYAML_MAPPING_STATE_VALUE,
};

/**
 * A CYAML load state machine stack entry.
 */
typedef struct cyaml_state {
	/** Current load state machine state. */
	enum cyaml_state_e state;
	/** Schema for the expected value in this state. */
	const cyaml_schema_type_t *schema;
	/** Anonymous union for schema type specific state. */
	union {
		/** Additional state for values of \ref CYAML_MAPPING type. */
		struct {
			const cyaml_schema_mapping_t *schema;
			/** Mapping load state machine sub-state. */
			enum cyaml_mapping_state_e state;
			uint16_t schema_idx;
			uint16_t entries_count;
		} mapping;
		/**
		 * Additional state for values of \ref CYAML_SEQUENCE and
		 * \ref CYAML_SEQUENCE_FIXED types.
		 */
		struct {
			uint8_t *data;
			uint8_t *count_data;
			uint32_t count;
			uint64_t count_size;
		} sequence;
	};
	uint8_t *data;
} cyaml_state_t;

/**
 * Internal YAML loading context.
 */
typedef struct cyaml_ctx {
	const cyaml_config_t *config; /**< Settings provided by client. */
	cyaml_state_t *state;   /**< Current entry in state stack, or NULL. */
	cyaml_state_t *stack;   /**< State stack */
	uint32_t stack_idx;     /**< Next (empty) state stack slot */
	uint32_t stack_max;     /**< Current stack allocation limit. */
	yaml_parser_t *parser;  /**< Internal libyaml parser object. */
} cyaml_ctx_t;

/**
 * CYAML events.  These are a flags which correspond to `libyaml` events.
 */
typedef enum cyaml_event {
	CYAML_EVT_NONE          = 0,
	CYAML_EVT_NO_EVENT      = (1 << YAML_NO_EVENT),
	CYAML_EVT_STREAM_START  = (1 << YAML_STREAM_START_EVENT),
	CYAML_EVT_STREAM_END    = (1 << YAML_STREAM_END_EVENT),
	CYAML_EVT_DOC_START     = (1 << YAML_DOCUMENT_START_EVENT),
	CYAML_EVT_DOC_END       = (1 << YAML_DOCUMENT_END_EVENT),
	CYAML_EVT_ALIAS         = (1 << YAML_ALIAS_EVENT),
	CYAML_EVT_SCALAR        = (1 << YAML_SCALAR_EVENT),
	CYAML_EVT_SEQ_START     = (1 << YAML_SEQUENCE_START_EVENT),
	CYAML_EVT_SEQ_END       = (1 << YAML_SEQUENCE_END_EVENT),
	CYAML_EVT_MAPPING_START = (1 << YAML_MAPPING_START_EVENT),
	CYAML_EVT_MAPPING_END   = (1 << YAML_MAPPING_END_EVENT),
} cyaml_event_t;

/**
 * Get the CYAML event type from a `libyaml` event.
 *
 * \param[in]  event  The `libyaml` event.
 * \return corresponding CYAML event.
 */
static inline cyaml_event_t cyaml__get_event_type(const yaml_event_t *event)
{
	return (1 << event->type);
}

/**
 * Convert a `libyaml` event to a human readable string.
 *
 * \param[in]  event  The `libyaml` event.
 * \return String representing event.
 */
static const char * cyaml__libyaml_event_type_str(const yaml_event_t *event)
{
	static const char *strings[] = {
		"NO_EVENT",
		"STREAM_START",
		"STREAM_END",
		"DOC_START",
		"DOC_END",
		"ALIAS",
		"SCALAR",
		"SEQUENCE_START",
		"SEQUENCE_END",
		"MAPPING_START",
		"MAPPING_END",
	};
	return strings[event->type];
}

/**
 * Helper function to read the next YAML input event.
 *
 * This gets the next event from the CYAML load context's `libyaml` parser
 * object.  It allows the caller to provide a mask of expected events.
 *
 * \param[in]  ctx    The CYAML loading context.
 * \param[in]  mask   Mask of expected event types.
 * \param[out] event  On success, returns the new event.
 * \return \ref CYAML_OK on success,
 *         \ref CYAML_ERR_UNEXPECTED_EVENT if event type is not in `mask`,
 *         or appropriate error otherwise.
 */
static cyaml_err_t cyaml_get_next_event(
		const cyaml_ctx_t *ctx,
		cyaml_event_t mask,
		yaml_event_t *event)
{
	if (!yaml_parser_parse(ctx->parser, event)) {
		cyaml__log(ctx->config, CYAML_LOG_ERROR,
				"libyaml: %s\n", ctx->parser->problem);
		return CYAML_ERR_LIBYAML_PARSER;
	}

	if (event->type == YAML_ALIAS_EVENT) {
		/** \todo Add support for alias? */
		yaml_event_delete(event);
		return CYAML_ERR_ALIAS;
	}

	if (!(cyaml__get_event_type(event) & mask)) {
		cyaml__log(ctx->config, CYAML_LOG_ERROR,
				"Unexpected event: %s\n",
				cyaml__libyaml_event_type_str(event));
		yaml_event_delete(event);
		return CYAML_ERR_UNEXPECTED_EVENT;
	}

	cyaml__log(ctx->config, CYAML_LOG_DEBUG, "Event: %s\n",
			cyaml__libyaml_event_type_str(event));

	return CYAML_OK;
}

/**
 * Convert a CYAML load state into a human readable string.
 *
 * \param[in]  state  The state to convert.
 * \return String representing state.
 */
static inline const char * cyaml__state_to_str(enum cyaml_state_e state)
{
	static const char * const strings[CYAML_STATE__COUNT] = {
		[CYAML_STATE_START]       = "start",
		[CYAML_STATE_IN_STREAM]   = "in stream",
		[CYAML_STATE_IN_DOC]      = "in doc",
		[CYAML_STATE_IN_MAPPING]  = "in mapping",
		[CYAML_STATE_IN_SEQUENCE] = "in sequence",
	};
	if ((unsigned)state >= CYAML_STATE__COUNT) {
		return "<invalid>";
	}
	return strings[state];
}

/**
 * Get the offset to a mapping field by key in a mapping schema array.
 *
 * \param[in]  mapping_schema  Array of mapping schema fields.
 * \param[in]  key             Key to search for in mapping schema.
 * \return index into `mapping_schema` for key, or \ref CYAML_SCHEMA_IDX_NONE
 *         if key is not present in `mapping_schema`.
 */
static inline uint16_t cyaml__get_entry_from_mapping_schema(
		const cyaml_schema_mapping_t * mapping_schema,
		const char *key)
{
	uint16_t index = 0;

	/* Step through each entry in the schema */
	for (; mapping_schema->key != NULL; mapping_schema++) {
		if (strcmp(mapping_schema->key, key) == 0) {
			return index;
		}
		index++;
	}

	return CYAML_SCHEMA_IDX_NONE;
}

/**
 * Ensure that the CYAML load context has space for a new stack entry.
 *
 * \param[in]  ctx     The CYAML loading context.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__stack_ensure(
		cyaml_ctx_t *ctx)
{
	cyaml_state_t *temp;
	uint32_t max = ctx->stack_max + 16;

	if (ctx->stack_idx < ctx->stack_max) {
		return CYAML_OK;
	}

	temp = realloc(ctx->stack, sizeof(*ctx->stack) * max);
	if (temp == NULL) {
		return CYAML_ERR_OOM;
	}

	ctx->stack = temp;
	ctx->stack_max = max;
	ctx->state = ctx->stack + ctx->stack_idx - 1;

	return CYAML_OK;
}

/**
 * Push a new entry onto the CYAML load context's stack.
 *
 * \param[in]  ctx     The CYAML loading context.
 * \param[in]  state   The CYAML load state we're pushing a stack entry for.
 * \param[in]  schema  The CYAML schema for the value expected in state.
 * \param[in]  data    Pointer to where value's data should be written.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__stack_push(
		cyaml_ctx_t *ctx,
		enum cyaml_state_e state,
		const cyaml_schema_type_t *schema,
		cyaml_data_t *data)
{
	cyaml_err_t err;
	cyaml_state_t s = {
		.data = data,
		.state = state,
		.schema = schema,
	};

	err = cyaml__stack_ensure(ctx);
	if (err != CYAML_OK) {
		return err;
	}

	switch (state) {
	case CYAML_STATE_IN_MAPPING:
		assert(schema->type == CYAML_MAPPING);
		s.mapping.schema = schema->mapping.schema;
		s.mapping.state = CYAML_MAPPING_STATE_KEY;
		break;
	case CYAML_STATE_IN_SEQUENCE:
		switch (schema->type) {
		case CYAML_SEQUENCE_FIXED:
			/* Fall through. */
		case CYAML_SEQUENCE:
			s.sequence.count_data = ctx->state->data +
					schema->sequence.count_offset;
			s.sequence.count_size = schema->sequence.count_size;
			break;
		default:
			return CYAML_ERR_INTERNAL_ERROR;
		}
		break;
	default:
		break;
	}

	cyaml__log(ctx->config, CYAML_LOG_DEBUG,
			"PUSH[%u]: %s\n", ctx->stack_idx,
			cyaml__state_to_str(state));

	ctx->stack[ctx->stack_idx] = s;
	ctx->state = ctx->stack + ctx->stack_idx;
	ctx->stack_idx++;

	return CYAML_OK;
}

/**
 * Pop the current entry on the CYAML load context's stack.
 *
 * This frees any resources owned by the stack entry.
 *
 * \param[in]  ctx     The CYAML loading context.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__stack_pop(
		cyaml_ctx_t *ctx)
{
	uint32_t idx = ctx->stack_idx;

	if (idx == 0) {
		return CYAML_ERR_INTERNAL_ERROR;
	}

	idx--;

	cyaml__log(ctx->config, CYAML_LOG_DEBUG, "POP[%u]: %s\n", idx,
			cyaml__state_to_str(ctx->state->state));

	ctx->state = (idx == 0) ? NULL : &ctx->stack[idx - 1];
	ctx->stack_idx = idx;

	return CYAML_OK;
}

/**
 * Helper to check if our current state is \ref CYAML_STATE_IN_SEQUENCE.
 *
 * \param[in]  ctx    The CYAML loading context.
 * \return true iff current state is \ref CYAML_STATE_IN_SEQUENCE,
 *         false otherwise.
 */
static inline bool cyaml_in_sequence(cyaml_ctx_t *ctx)
{
	return ((ctx != NULL) &&
	        (ctx->state != NULL) &&
	        (ctx->state->state == CYAML_STATE_IN_SEQUENCE));
}

/**
 * Helper to make allocations for loaded YAML values.
 *
 * If the current state is sequence, this extends any existing allocation
 * for the sequence.
 *
 * The current CYAML loading context's state is updated with new allocation
 * address, where necessary.
 *
 * \param[in]      ctx            The CYAML loading context.
 * \param[in]      schema         The schema for value to get data pointer for.
 * \param[in]      event          The YAML event value to get data pointer for.
 * \param[in,out]  value_data_io  Current address of value's data.  Updated to
 *                                new address if value is allocation requiring
 *                                an allocation.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__data_handle_pointer(
		cyaml_ctx_t *ctx,
		const cyaml_schema_type_t *schema,
		const yaml_event_t *event,
		uint8_t **value_data_io)
{
	cyaml_err_t err = CYAML_OK;
	cyaml_state_t *state = ctx->state;

	if (schema->flags & CYAML_FLAG_POINTER) {
		/* Need to create/extend an allocation. */
		size_t delta = schema->data_size;
		uint8_t *value_data = NULL;
		size_t offset = 0;

		if (schema->type == CYAML_STRING) {
			/* For a string the allocation size is the string
			 * size from the event, plus trailing NULL. */
			delta = strlen((const char *)
					event->data.scalar.value) + 1;
		}

		if (cyaml_in_sequence(ctx)) {
			/* Sequence; could be extending allocation. */
			offset = schema->data_size * state->sequence.count;
			value_data = state->sequence.data;
		}
		value_data = realloc(value_data, offset + delta);
		if (value_data == NULL) {
			return CYAML_ERR_OOM;
		}
		memset(value_data + offset, 0, delta);

		if (cyaml_in_sequence(ctx)) {
			/* Updated the in sequence state so it knows the new
			 * allocation address. */
			state->sequence.data = value_data;
		}

		/* Write the allocation pointer into the data structure. */
		err = cyaml_data_write((uint64_t)value_data,
				sizeof(value_data), *value_data_io);
		if (err != CYAML_OK) {
			return err;
		}

		*value_data_io = value_data;
	}

	return err;
}

/**
 * Read a value of type \ref CYAML_INT.
 *
 * \param[in]  ctx     The CYAML loading context.
 * \param[in]  schema  The schema for the value to be read.
 * \param[in]  value   String containing scaler value.
 * \param[in]  data    The place to write the value in the output data.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__read_int(
		const cyaml_ctx_t *ctx,
		const cyaml_schema_type_t *schema,
		const char *value,
		uint8_t *data)
{
	char *end = NULL;
	long long temp = strtoll(value, &end, 0);
	int64_t max = ((~(uint64_t)0) >> ((8 - schema->data_size) * 8)) / 2;
	int64_t min = (-max) - 1;

	CYAML_UNUSED(ctx);

	if (end == value || errno == ERANGE ||
	    temp < min || temp > max) {
		return CYAML_ERR_INVALID_VALUE;
	}

	return cyaml_data_write(temp, schema->data_size, data);
}

/**
 * Read a scalar value.
 *
 * \param[in]  ctx     The CYAML loading context.
 * \param[in]  schema  The schema for the value to be read.
 * \param[in]  data    The place to write the value in the output data.
 * \param[in]  event   The `libyaml` event providing the scalar value data.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__read_scalar_value(
		const cyaml_ctx_t *ctx,
		const cyaml_schema_type_t *schema,
		cyaml_data_t *data,
		yaml_event_t *event)
{
	const char *value = (const char *)event->data.scalar.value;
	typedef cyaml_err_t (*cyaml_read_scalar_fn)(
			const cyaml_ctx_t *ctx,
			const cyaml_schema_type_t *schema,
			const char *value,
			uint8_t *data_target);
	static const cyaml_read_scalar_fn fn[CYAML__TYPE_COUNT] = {
		[CYAML_INT]    = cyaml__read_int,
	};

	cyaml__log(ctx->config, CYAML_LOG_INFO, "  <%s>\n", value);

	assert(fn[schema->type] != NULL);

	return fn[schema->type](ctx, schema, value, data);
}

/**
 * Handle a YAML event corresponding to a YAML data value.
 *
 * \param[in]  ctx     The CYAML loading context.
 * \param[in]  schema  CYAML schema for the expected value.
 * \param[in]  data    Pointer to where value's data should be written.
 * \param[in]  event   The YAML event to handle.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__read_value(
		cyaml_ctx_t *ctx,
		const cyaml_schema_type_t *schema,
		uint8_t *data,
		yaml_event_t *event)
{
	cyaml_event_t cyaml_event = cyaml__get_event_type(event);
	cyaml_err_t err = CYAML_OK;

	if ((schema->type != CYAML_SEQUENCE) &&
	    (schema->type != CYAML_SEQUENCE_FIXED)) {
		/* Since sequences extend their allocation for each entry,
		 * the're handled in the sequence-specific code.
		 */
		cyaml__data_handle_pointer(ctx, schema, event, &data);
	}

	switch (schema->type) {
	case CYAML_INT:  /* Fall through. */
	case CYAML_UINT: /* Fall through. */
	case CYAML_BOOL: /* Fall through. */
	case CYAML_ENUM: /* Fall through. */
	case CYAML_STRING:
		if (cyaml_event != CYAML_EVT_SCALAR) {
			return CYAML_ERR_INVALID_VALUE;
		}
		err = cyaml__read_scalar_value(ctx, schema, data, event);
		break;
	case CYAML_FLAGS:
		/** \todo */
		break;
	case CYAML_MAPPING:
		if (cyaml_event != CYAML_EVT_MAPPING_START) {
			return CYAML_ERR_INVALID_VALUE;
		}
		err = cyaml__stack_push(ctx, CYAML_STATE_IN_MAPPING,
				schema, data);
		break;
	case CYAML_SEQUENCE: /* Fall through. */
	case CYAML_SEQUENCE_FIXED:
		if (cyaml_event != CYAML_EVT_SEQ_START) {
			cyaml__log(ctx->config, CYAML_LOG_ERROR,
					"Unexpected event: %s\n",
					cyaml__libyaml_event_type_str(event));
			return CYAML_ERR_INVALID_VALUE;
		}
		err = cyaml__stack_push(ctx, CYAML_STATE_IN_SEQUENCE,
				schema, data);
		break;
	case CYAML_IGNORE:
		/** \todo */
		break;
	default:
		err = CYAML_ERR_BAD_TYPE_IN_SCHEMA;
		break;
	}

	return err;
}

/**
 * Handle a YAML event adding a new entry to a sequence.
 *
 * \param[in]  ctx    The CYAML loading context.
 * \param[in]  event  The YAML event to handle.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__new_sequence_entry(
		cyaml_ctx_t *ctx,
		yaml_event_t *event)
{
	cyaml_err_t err;
	cyaml_state_t *state = ctx->state;
	uint8_t *value_data = state->data;
	const cyaml_schema_type_t *schema = state->schema;

	err = cyaml__data_handle_pointer(ctx, schema, event, &value_data);
	if (err != CYAML_OK) {
		return err;
	}

	cyaml__log(ctx->config, CYAML_LOG_DEBUG,
			"Sequence entry: %u (%"PRIu32" bytes)\n",
			state->sequence.count, schema->data_size);
	value_data += schema->data_size * state->sequence.count;
	state->sequence.count++;

	if (schema->type != CYAML_SEQUENCE_FIXED) {
		err = cyaml_data_write(state->sequence.count,
				state->sequence.count_size,
				state->sequence.count_data);
		if (err != CYAML_OK) {
			cyaml__log(ctx->config, CYAML_LOG_ERROR,
					"Failed writing sequence count\n",
					state->sequence.count,
					schema->data_size);
			return err;
		}
	}

	/* Read the actual value */
	err = cyaml__read_value(ctx, schema->sequence.schema,
			value_data, event);
	if (err != CYAML_OK) {
		return err;
	}

	return CYAML_OK;
}

/**
 * YAML loading handler for the \ref CYAML_STATE_START state.
 *
 * \param[in]  ctx  The CYAML loading context.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__read_start(
		cyaml_ctx_t *ctx)
{
	cyaml_err_t err;
	yaml_event_t event;
	cyaml_event_t mask = CYAML_EVT_STREAM_START;

	err = cyaml_get_next_event(ctx, mask, &event);
	if (err != CYAML_OK) {
		return err;
	}

	switch (cyaml__get_event_type(&event)) {
	case CYAML_EVT_STREAM_START:
		err = cyaml__stack_push(ctx, CYAML_STATE_IN_STREAM,
				ctx->state->schema, ctx->state->data);
		break;
	default:
		assert(mask & cyaml__get_event_type(&event));
		break;
	}

	if (err != CYAML_OK) {
		goto out;
	}

out:
	yaml_event_delete(&event);
	return err;
}

/**
 * YAML loading handler for the \ref CYAML_STATE_IN_STREAM state.
 *
 * \param[in]  ctx  The CYAML loading context.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__read_stream(
		cyaml_ctx_t *ctx)
{
	cyaml_err_t err;
	yaml_event_t event;
	cyaml_event_t mask = CYAML_EVT_DOC_START | CYAML_EVT_STREAM_END;

	err = cyaml_get_next_event(ctx, mask, &event);
	if (err != CYAML_OK) {
		return err;
	}

	switch (cyaml__get_event_type(&event)) {
	case CYAML_EVT_DOC_START:
		err = cyaml__stack_push(ctx, CYAML_STATE_IN_DOC,
				ctx->state->schema, ctx->state->data);
		break;
	case CYAML_EVT_STREAM_END:
		err = cyaml__stack_pop(ctx);
		break;
	default:
		assert(mask & cyaml__get_event_type(&event));
		break;
	}

	if (err != CYAML_OK) {
		goto out;
	}

out:
	yaml_event_delete(&event);
	return err;
}

/**
 * YAML loading handler for the \ref CYAML_STATE_IN_DOC state.
 *
 * \param[in]  ctx  The CYAML loading context.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__read_doc(
		cyaml_ctx_t *ctx)
{
	cyaml_err_t err;
	yaml_event_t event;
	cyaml_event_t mask = CYAML_EVT_MAPPING_START | CYAML_EVT_DOC_END;

	err = cyaml_get_next_event(ctx, mask, &event);
	if (err != CYAML_OK) {
		return err;
	}

	switch (cyaml__get_event_type(&event)) {
	case CYAML_EVT_MAPPING_START:
		err = cyaml__stack_push(ctx, CYAML_STATE_IN_MAPPING,
				ctx->state->schema, ctx->state->data);
		break;
	case CYAML_EVT_DOC_END:
		err = cyaml__stack_pop(ctx);
		break;
	default:
		assert(mask & cyaml__get_event_type(&event));
		break;
	}

	if (err != CYAML_OK) {
		goto out;
	}

out:
	yaml_event_delete(&event);
	return err;
}

/**
 * YAML loading handler for the \ref CYAML_MAPPING_STATE_KEY sub-state of
 * the \ref CYAML_STATE_IN_MAPPING state.
 *
 * \param[in]  ctx  The CYAML loading context.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__read_mapping_key(
		cyaml_ctx_t *ctx)
{
	const char *key;
	cyaml_err_t err;
	yaml_event_t event;
	cyaml_event_t cyaml_event;
	cyaml_event_t mask = CYAML_EVT_SCALAR | CYAML_EVT_MAPPING_END;

	err = cyaml_get_next_event(ctx, mask, &event);
	if (err != CYAML_OK) {
		return err;
	}
	cyaml_event = cyaml__get_event_type(&event);

	switch (cyaml_event) {
	case CYAML_EVT_SCALAR:
		key = (const char *)event.data.scalar.value;
		ctx->state->mapping.schema_idx =
				cyaml__get_entry_from_mapping_schema(
					ctx->state->mapping.schema, key);
		cyaml__log(ctx->config, CYAML_LOG_INFO, "[%s]\n", key);

		if (ctx->state->mapping.schema_idx == CYAML_SCHEMA_IDX_NONE) {
			err = CYAML_ERR_INVALID_KEY;
			goto out;
		}
		/* Toggle mapping sub-state to value */
		ctx->state->mapping.state = CYAML_MAPPING_STATE_VALUE;
		break;
	case CYAML_EVT_MAPPING_END:
		err = cyaml__stack_pop(ctx);
		if (err != CYAML_OK) {
			goto out;
		}
		break;
	default:
		assert(mask & cyaml_event);
		break;
	}

out:
	yaml_event_delete(&event);
	return err;
}

/**
 * YAML loading handler for the \ref CYAML_MAPPING_STATE_VALUE sub-state of
 * the \ref CYAML_STATE_IN_MAPPING state.
 *
 * \param[in]  ctx  The CYAML loading context.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__read_mapping_value(
		cyaml_ctx_t *ctx)
{
	cyaml_err_t err;
	yaml_event_t event;
	cyaml_state_t *state = ctx->state;
	cyaml_event_t mask = CYAML_EVT_SCALAR |
	                     CYAML_EVT_SEQ_START |
	                     CYAML_EVT_MAPPING_START;
	const cyaml_schema_mapping_t *entry =
			state->mapping.schema + state->mapping.schema_idx;
	cyaml_data_t *data = state->data + entry->data_offset;

	err = cyaml_get_next_event(ctx, mask, &event);
	if (err != CYAML_OK) {
		return err;
	}

	/* Toggle mapping sub-state back to key.  Do this before
	 * reading value, because reading value might increase the
	 * CYAML context stack allocation, causing the state entry
	 * to move. */
	state->mapping.state = CYAML_MAPPING_STATE_KEY;

	err = cyaml__read_value(ctx, &entry->value, data, &event);
	if (err != CYAML_OK) {
		goto out;
	}

out:
	yaml_event_delete(&event);
	return err;
}

/**
 * YAML loading handler for the \ref CYAML_STATE_IN_MAPPING state.
 *
 * \param[in]  ctx  The CYAML loading context.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__read_mapping(
		cyaml_ctx_t *ctx)
{
	/* Mapping has two sub-states; key and value */
	switch (ctx->state->mapping.state) {
	case CYAML_MAPPING_STATE_KEY:
		return cyaml__read_mapping_key(ctx);
	case CYAML_MAPPING_STATE_VALUE:
		return cyaml__read_mapping_value(ctx);
	}

	return CYAML_ERR_INTERNAL_ERROR;
}

/**
 * YAML loading handler for the \ref CYAML_STATE_IN_SEQUENCE state.
 *
 * \param[in]  ctx  The CYAML loading context.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__read_sequence(
		cyaml_ctx_t *ctx)
{
	cyaml_err_t err;
	yaml_event_t event;
	cyaml_state_t *state = ctx->state;
	cyaml_event_t mask = CYAML_EVT_MAPPING_START |
	                     CYAML_EVT_SEQ_START |
	                     CYAML_EVT_SEQ_END |
	                     CYAML_EVT_SCALAR;

	err = cyaml_get_next_event(ctx, mask, &event);
	if (err != CYAML_OK) {
		return err;
	}

	switch (cyaml__get_event_type(&event)) {
	case CYAML_EVT_SCALAR:    /* Fall through. */
	case CYAML_EVT_SEQ_START: /* Fall through. */
	case CYAML_EVT_MAPPING_START:
		err = cyaml__new_sequence_entry(ctx, &event);
		break;
	case CYAML_EVT_SEQ_END:
		cyaml__log(ctx->config, CYAML_LOG_DEBUG, "Sequence count: %u\n",
				state->sequence.count);
		err = cyaml__stack_pop(ctx);
		break;
	default:
		err = CYAML_ERR_INTERNAL_ERROR;
		break;
	}

	if (err != CYAML_OK) {
		goto out;
	}

out:
	yaml_event_delete(&event);
	return err;
}

/**
 * The main YAML loading function.
 *
 * The public interfaces are wrappers around this.
 *
 * \param[in]  config     Client's CYAML configuration structure.
 * \param[in]  schema     CYAML schema for the YAML to be loaded.
 * \param[out] data_out   Returns the caller-owned loaded data on success.
 *                        Untouched on failure.
 * \param[in]  parser     An initialised `libyaml` parser object with input set.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static cyaml_err_t cyaml__load(
		const cyaml_config_t *config,
		const cyaml_schema_type_t *schema,
		cyaml_data_t **data_out,
		yaml_parser_t *parser)
{
	cyaml_data_t *data = NULL;
	cyaml_ctx_t ctx = {
		.config = config,
		.parser = parser,
	};
	typedef cyaml_err_t (*cyaml_read_fn)(
			cyaml_ctx_t *ctx);
	static const cyaml_read_fn fn[CYAML_STATE__COUNT] = {
		[CYAML_STATE_START]       = cyaml__read_start,
		[CYAML_STATE_IN_STREAM]   = cyaml__read_stream,
		[CYAML_STATE_IN_DOC]      = cyaml__read_doc,
		[CYAML_STATE_IN_MAPPING]  = cyaml__read_mapping,
		[CYAML_STATE_IN_SEQUENCE] = cyaml__read_sequence,
	};
	cyaml_err_t err = CYAML_OK;

	data = calloc(1, schema->data_size);
	if (data == NULL) {
		return CYAML_ERR_OOM;
	}

	err = cyaml__stack_push(&ctx, CYAML_STATE_START, schema, data);
	if (err != CYAML_OK) {
		goto out;
	}
	ctx.state->data = data;

	do {
		cyaml__log(ctx.config, CYAML_LOG_DEBUG, "Handle state %s\n",
				cyaml__state_to_str(ctx.state->state));
		err = fn[ctx.state->state](&ctx);
		if (err != CYAML_OK) {
			goto out;
		}
	} while (ctx.state->state > CYAML_STATE_START);

	err = cyaml__stack_pop(&ctx);
	if (err != CYAML_OK) {
		goto out;
	}

	assert(ctx.stack_idx == 0);

	*data_out = data;
out:
	if (err != CYAML_OK) {
		cyaml_free(config, schema, data);
	}
	while (ctx.stack_idx > 0) {
		cyaml__stack_pop(&ctx);
	}
	free(ctx.stack);
	return err;
}

/**
 * Check that common load params from client are valid.
 *
 * \param[in] config    The client's CYAML library config.
 * \param[in] schema    The schema describing the content of data.
 * \param[in] data_tgt  Points to client's address to write data to.
 * \return \ref CYAML_OK on success, or appropriate error code otherwise.
 */
static inline cyaml_err_t cyaml__validate_load_params(
		const cyaml_config_t *config,
		const cyaml_schema_type_t *schema,
		cyaml_data_t * const *data_tgt)
{
	if (config == NULL) {
		return CYAML_ERR_BAD_PARAM_NULL_CONFIG;
	}
	if (schema == NULL) {
		return CYAML_ERR_BAD_PARAM_NULL_SCHEMA;
	}
	if (schema->type != CYAML_MAPPING) {
		return CYAML_ERR_BAD_TOP_LEVEL_TYPE;
	}
	if (data_tgt == NULL) {
		return CYAML_ERR_BAD_PARAM_NULL_DATA;
	}
	return CYAML_OK;
}

/* Exported function, documented in include/cyaml/cyaml.h */
cyaml_err_t cyaml_load_file(
		const char *path,
		const cyaml_config_t *config,
		const cyaml_schema_type_t *schema,
		cyaml_data_t **data_out)
{
	FILE *file;
	cyaml_err_t err;
	yaml_parser_t parser;

	err = cyaml__validate_load_params(config, schema, data_out);
	if (err != CYAML_OK) {
		return err;
	}

	file = fopen(path, "r");
	if (file == NULL) {
		return CYAML_ERR_FILE_OPEN;
	}

	/* Initialize parser */
	if (!yaml_parser_initialize(&parser)) {
		fclose(file);
		return CYAML_ERR_LIBYAML_PARSER_INIT;
	}

	/* Set input file */
	yaml_parser_set_input_file(&parser, file);

	/* Parse the input */
	err = cyaml__load(config, schema, data_out, &parser);
	if (err != CYAML_OK) {
		yaml_parser_delete(&parser);
		fclose(file);
		return err;
	}

	/* Cleanup */
	yaml_parser_delete(&parser);
	fclose(file);

	return CYAML_OK;
}

/* Exported function, documented in include/cyaml/cyaml.h */
cyaml_err_t cyaml_load_data(
		const uint8_t *input,
		size_t input_len,
		const cyaml_config_t *config,
		const cyaml_schema_type_t *schema,
		cyaml_data_t **data_out)
{
	cyaml_err_t err;
	yaml_parser_t parser;

	err = cyaml__validate_load_params(config, schema, data_out);
	if (err != CYAML_OK) {
		return err;
	}

	/* Initialize parser */
	if (!yaml_parser_initialize(&parser)) {
		return CYAML_ERR_LIBYAML_PARSER_INIT;
	}

	/* Set input data */
	yaml_parser_set_input_string(&parser, input, input_len);

	/* Parse the input */
	err = cyaml__load(config, schema, data_out, &parser);
	if (err != CYAML_OK) {
		yaml_parser_delete(&parser);
		return err;
	}

	/* Cleanup */
	yaml_parser_delete(&parser);

	return CYAML_OK;
}