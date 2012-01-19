/*
 * Copyright 2011-2012 Luc Verhaegen <libv@codethink.co.uk>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */
/*
 * Dealing with shader programs, from compilation to linking.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "premali.h"
#include "plb.h"
#include "ioctl.h"
#include "gp.h"
#include "program.h"
#include "compiler.h"
#include "symbols.h"

/*
 * Attribute linking:
 *
 * One single attribute per command:
 *  bits 0x1C.2 through 0x1C.5 of every instruction,
 *  bit 0x1C.6 is set when there is an attribute.
 *
 * Before linking, attribute 0 is aColor, and attribute 1 is aPosition,
 * as obviously spotted from the attribute link stream.
 * After linking, attribute 0 is aVertices, and attribute 1 is aColor.
 * This matches the way in which we attach our attributes.
 *
 * And indeed, if we swap the bits and the attributes over, things work fine,
 * and if we only swap the attributes over, then the rendering is broken in a
 * logical way.
 */
/*
 * Varying linking:
 *
 * 4 entries, linked to two varyings. If both varyings are specified, they have
 * to be the same.
 *
 * Bits 47-52, contain 4 entries, 3 bits each. 7 means invalid. 2 entries per
 * varying.
 * Bits 5A through 63 specify two varyings, 5 bits each. Top bit means
 * valid/defined, other is most likely the index in the common area.
 *
 * How this fully fits together, is not entirely clear yet, but hopefully
 * clear enough to implement linking in one of the next stages.
 */
/*
 * Uniform linking:
 *
 * Since there is just a memory blob with uniforms packed in it, it would
 * appear that there is no linking, and uniforms just have to be packed as
 * defined in the uniform link stream.
 *
 */

#define STREAM_TAG_STRI 0x49525453

#define STREAM_TAG_SUNI 0x494E5553
#define STREAM_TAG_VUNI 0x494E5556
#define STREAM_TAG_VINI 0x494E4956

#define STREAM_TAG_SATT 0x54544153
#define STREAM_TAG_VATT 0x54544156

struct stream_string {
	unsigned int tag; /* STRI */
	int size;
	char string[];
};

struct stream_uniform_table_start {
	unsigned int tag; /* SUNI */
	int size;
	int count;
	int space_needed;
};

struct stream_uniform_start {
	unsigned int tag; /* VUNI */
	int size;
};

struct stream_uniform_data { /* 0x14 */
	unsigned char type; /* 0x00 */
	unsigned char unknown01; /* 0x01 */
	unsigned short element_count; /* 0x02 */
	unsigned short element_size; /* 0x04 */
	unsigned short entry_count; /* 0x06. 0 == 1 */
	unsigned short stride; /* 0x08 */
	unsigned char unknown0A; /* 0x0A */
	unsigned char precision; /* 0x0B */
	unsigned short unknown0C; /* 0x0C */
	unsigned short unknown0E; /* 0x0E */
	unsigned short offset; /* offset into the uniform memory */
	unsigned short index; /* often -1 */
};

struct stream_uniform_init {
	unsigned int tag; /* VINI */
	unsigned int size;
	unsigned int count;
	unsigned int data[];
};

struct stream_uniform {
	struct stream_uniform *next;

	struct stream_uniform_start *start;
	struct stream_string *string;
	struct stream_uniform_data *data;
	struct stream_uniform_init *init;
};

struct stream_uniform_table {
	struct stream_uniform_table_start *start;

	struct stream_uniform *uniforms;
};

static int
stream_uniform_table_start_read(void *stream, struct stream_uniform_table *table)
{
	struct stream_uniform_table_start *start = stream;

	if (start->tag != STREAM_TAG_SUNI)
		return 0;

	table->start = start;
	return sizeof(struct stream_uniform_table_start);
}

static int
stream_uniform_start_read(void *stream, struct stream_uniform *uniform)
{
	struct stream_uniform_start *start = stream;

	if (start->tag != STREAM_TAG_VUNI)
		return 0;

	uniform->start = start;
	return sizeof(struct stream_uniform_start);
}

static int
stream_string_read(void *stream, struct stream_string **string)
{
	*string = stream;

	if ((*string)->tag == STREAM_TAG_STRI)
		return sizeof(struct stream_string) + (*string)->size;

	*string = NULL;
	return 0;
}

static int
stream_uniform_data_read(void *stream, struct stream_uniform *uniform)
{
	uniform->data = stream;

	return sizeof(struct stream_uniform_data);
}

static int
stream_uniform_init_read(void *stream, struct stream_uniform *uniform)
{
	struct stream_uniform_init *init = stream;

	if (init->tag != STREAM_TAG_VINI)
		return 0;

	uniform->init = init;

	return 8 + init->size;
}

static void
stream_uniform_table_destroy(struct stream_uniform_table *table)
{
	if (table) {
		while (table->uniforms) {
			struct stream_uniform *uniform = table->uniforms;

			table->uniforms = uniform->next;
			free(uniform);
		}

		free(table);
	}
}

static struct stream_uniform_table *
stream_uniform_table_create(void *stream, int size)
{
	struct stream_uniform_table *table;
	struct stream_uniform *uniform;
	int offset = 0;
	int i;

	if (!stream || !size)
		return NULL;

	table = calloc(1, sizeof(struct stream_uniform_table));
	if (!table) {
		printf("%s: failed to allocate table\n", __func__);
		return NULL;
	}

	offset += stream_uniform_table_start_read(stream + offset, table);
	if (!table->start) {
		printf("%s: Error: missing table start at 0x%x\n",
		       __func__, offset);
		goto corrupt;
	}

	for (i = 0; i < table->start->count; i++) {
		uniform = calloc(1, sizeof(struct stream_uniform));
		if (!table) {
			printf("%s: failed to allocate uniform\n", __func__);
			goto oom;
		}

		offset += stream_uniform_start_read(stream + offset, uniform);
		if (!uniform->start) {
			printf("%s: Error: missing uniform start at 0x%x\n",
			       __func__, offset);
			goto corrupt;
		}

		offset += stream_string_read(stream + offset, &uniform->string);
		if (!uniform->string) {
			printf("%s: Error: missing string at 0x%x\n",
			       __func__, offset);
			goto corrupt;
		}

		offset += stream_uniform_data_read(stream + offset, uniform);
		if (!uniform->data) {
			printf("%s: Error: missing uniform data at 0x%x\n",
			       __func__, offset);
			goto corrupt;
		}

		offset += stream_uniform_init_read(stream + offset, uniform);
		/* it is legal to not have an init block */

		uniform->next = table->uniforms;
		table->uniforms = uniform;
	}

	return table;
 oom:
	stream_uniform_table_destroy(table);
	return NULL;
 corrupt:
	stream_uniform_table_destroy(table);
	return NULL;
}

#if 0
static void
stream_uniform_table_print(struct stream_uniform_table *table)
{
	struct stream_uniform *uniform;

	printf("%s: Uniform space needed: 0x%x\n",
	       __func__, table->start->space_needed);
	uniform = table->uniforms;
	while (uniform) {
		printf("uniform \"%s\" = {\n", uniform->string->string);
		printf("\t type 0x%02x, unknown01 0x%02x, element_count 0x%04x\n",
		       uniform->data->type, uniform->data->unknown01,
		       uniform->data->element_count);
		printf("\t element_size 0x%04x, entry_count 0x%04x\n",
		       uniform->data->element_size, uniform->data->entry_count);
		printf("\t stride 0x%04x, unknown0A 0x%02x, precision 0x%02x\n",
		       uniform->data->stride, uniform->data->unknown0A,
		       uniform->data->precision);
		printf("\t unknown0C 0x%04x, unknown0E 0x%04x\n",
		       uniform->data->unknown0C, uniform->data->unknown0E);
		printf("\t offset 0x%04x, index 0x%04x\n",
		       uniform->data->offset, uniform->data->index);
		printf("}\n");
		uniform = uniform->next;
	}
}
#endif

static struct symbol **
stream_uniform_table_to_symbols(struct stream_uniform_table *table,
				int *count, int *size)
{
	struct stream_uniform *uniform;
	struct symbol **symbols;
	int i;

	if (!table || !count || !size)
		return NULL;

	*count = table->start->count;
	*size = table->start->space_needed;

	if (!table->start->count)
		return NULL;

	symbols = calloc(*count, sizeof(struct symbol *));
	if (!symbols) {
		printf("%s: Error: failed to allocate symbols: %s\n",
		       __func__, strerror(errno));
		goto error;
	}

	for (i = 0, uniform = table->uniforms;
	     (i < table->start->count) && uniform;
	     i++, uniform = uniform->next) {
		if (uniform->init)
			symbols[i] =
				symbol_create(uniform->string->string, SYMBOL_UNIFORM,
					      uniform->data->element_count * uniform->data->element_size,
					      uniform->data->element_count, uniform->data->entry_count,
					      uniform->init->data, 1);
		else
			symbols[i] =
				symbol_create(uniform->string->string, SYMBOL_UNIFORM,
					      uniform->data->element_count * uniform->data->element_size,
					      uniform->data->element_count, uniform->data->entry_count,
					      NULL, 0);
		if (!symbols[i]) {
			printf("%s: Error: failed to create symbol %s: %s\n",
			       __func__, uniform->string->string, strerror(errno));
			goto error;
		}

		symbols[i]->offset = uniform->data->offset;
	}

	return symbols;
 error:
	if (symbols) {
		for (i = 0; i < *count; i++)
			if (symbols[i])
				symbol_destroy(symbols[i]);
	}
	free(symbols);

	*count = 0;
	*size = 0;

	return NULL;
}

/*
 * Now for Attributes.
 */
struct stream_attribute_table_start {
	unsigned int tag; /* SATT */
	int size;
	int count;
};

struct stream_attribute_start {
	unsigned int tag; /* VATT */
	int size;
};

struct stream_attribute_data { /* 0x14 */
	unsigned char type; /* 0x00 */
	unsigned char unknown01; /* 0x01 */
	unsigned short element_count; /* 0x02 */
	unsigned short element_size; /* 0x04 */
	unsigned short entry_count; /* 0x06. 0 == 1 */
	unsigned short stride; /* 0x08 */
	unsigned char unknown0A; /* 0x0A */
	unsigned char precision; /* 0x0B */
	unsigned short unknown0C; /* 0x0C */
	unsigned short offset; /* 0x0E */
};

struct stream_attribute_init {
	unsigned int tag; /* VINI */
	unsigned int size;
	unsigned int count;
	unsigned int data[];
};

struct stream_attribute {
	struct stream_attribute *next;

	struct stream_attribute_start *start;
	struct stream_string *string;
	struct stream_attribute_data *data;
	struct stream_attribute_init *init;
};

struct stream_attribute_table {
	struct stream_attribute_table_start *start;

	struct stream_attribute *attributes;
};

static int
stream_attribute_table_start_read(void *stream, struct stream_attribute_table *table)
{
	struct stream_attribute_table_start *start = stream;

	if (start->tag != STREAM_TAG_SATT)
		return 0;

	table->start = start;
	return sizeof(struct stream_attribute_table_start);
}

static int
stream_attribute_start_read(void *stream, struct stream_attribute *attribute)
{
	struct stream_attribute_start *start = stream;

	if (start->tag != STREAM_TAG_VATT)
		return 0;

	attribute->start = start;
	return sizeof(struct stream_attribute_start);
}

static int
stream_attribute_data_read(void *stream, struct stream_attribute *attribute)
{
	attribute->data = stream;

	return sizeof(struct stream_attribute_data);
}

static void
stream_attribute_table_destroy(struct stream_attribute_table *table)
{
	if (table) {
		while (table->attributes) {
			struct stream_attribute *attribute = table->attributes;

			table->attributes = attribute->next;
			free(attribute);
		}

		free(table);
	}
}

static struct stream_attribute_table *
stream_attribute_table_create(void *stream, int size)
{
	struct stream_attribute_table *table;
	struct stream_attribute *attribute;
	int offset = 0;
	int i;

	if (!stream || !size)
		return NULL;

	table = calloc(1, sizeof(struct stream_attribute_table));
	if (!table) {
		printf("%s: failed to allocate table\n", __func__);
		return NULL;
	}

	offset += stream_attribute_table_start_read(stream + offset, table);
	if (!table->start) {
		printf("%s: Error: missing table start at 0x%x\n",
		       __func__, offset);
		goto corrupt;
	}

	for (i = 0; i < table->start->count; i++) {
		attribute = calloc(1, sizeof(struct stream_attribute));
		if (!table) {
			printf("%s: failed to allocate attribute\n", __func__);
			goto oom;
		}

		offset += stream_attribute_start_read(stream + offset, attribute);
		if (!attribute->start) {
			printf("%s: Error: missing attribute start at 0x%x\n",
			       __func__, offset);
			goto corrupt;
		}

		offset += stream_string_read(stream + offset, &attribute->string);
		if (!attribute->string) {
			printf("%s: Error: missing string at 0x%x\n",
			       __func__, offset);
			goto corrupt;
		}

		offset += stream_attribute_data_read(stream + offset, attribute);
		if (!attribute->data) {
			printf("%s: Error: missing attribute data at 0x%x\n",
			       __func__, offset);
			goto corrupt;
		}

		attribute->next = table->attributes;
		table->attributes = attribute;
	}

	return table;
 oom:
	stream_attribute_table_destroy(table);
	return NULL;
 corrupt:
	stream_attribute_table_destroy(table);
	return NULL;
}

#if 0
static void
stream_attribute_table_print(struct stream_attribute_table *table)
{
	struct stream_attribute *attribute;

	attribute = table->attributes;
	while (attribute) {
		printf("attribute \"%s\" = {\n", attribute->string->string);
		printf("\t type 0x%02x, unknown01 0x%02x, element_count 0x%04x\n",
		       attribute->data->type, attribute->data->unknown01,
		       attribute->data->element_count);
		printf("\t element_size 0x%04x, entry_count 0x%04x\n",
		       attribute->data->element_size, attribute->data->entry_count);
		printf("\t stride 0x%04x, unknown0A 0x%02x, precision 0x%02x\n",
		       attribute->data->stride, attribute->data->unknown0A,
		       attribute->data->precision);
		printf("\t unknown0C 0x%04x, offset 0x%04x\n",
		       attribute->data->unknown0C, attribute->data->offset);
		printf("}\n");
		attribute = attribute->next;
	}
}
#endif

static struct symbol **
stream_attribute_table_to_symbols(struct stream_attribute_table *table,
				int *count)
{
	struct stream_attribute *attribute;
	struct symbol **symbols;
	int i;

	if (!table || !count)
		return NULL;

	*count = table->start->count;

	if (!table->start->count)
		return NULL;

	symbols = calloc(*count, sizeof(struct symbol *));
	if (!symbols) {
		printf("%s: Error: failed to allocate symbols: %s\n",
		       __func__, strerror(errno));
		goto error;
	}

	for (i = 0, attribute = table->attributes;
	     (i < table->start->count) && attribute;
	     i++, attribute = attribute->next) {
		if (attribute->init)
			symbols[i] =
				symbol_create(attribute->string->string, SYMBOL_ATTRIBUTE,
					      attribute->data->element_count * attribute->data->element_size,
					      attribute->data->element_count, attribute->data->entry_count,
					      attribute->init->data, 1);
		else
			symbols[i] =
				symbol_create(attribute->string->string, SYMBOL_ATTRIBUTE,
					      attribute->data->element_count * attribute->data->element_size,
					      attribute->data->element_count, attribute->data->entry_count,
					      NULL, 0);
		if (!symbols[i]) {
			printf("%s: Error: failed to create symbol %s: %s\n",
			       __func__, attribute->string->string, strerror(errno));
			goto error;
		}

		symbols[i]->offset = attribute->data->offset;
	}

	return symbols;
 error:
	if (symbols) {
		for (i = 0; i < *count; i++)
			if (symbols[i])
				symbol_destroy(symbols[i]);
	}
	free(symbols);

	*count = 0;

	return NULL;
}

/*
 *
 */
static void
vertex_shader_attributes_patch(unsigned int *shader, int size)
{
	int i;

	for (i = 0; i < size; i++) {
		int tmp = (shader[4 * i + 1] >> 26) & 0x1F;

		if (!(tmp & 0x10))
			continue;

		tmp &= 0x0F;
		shader[4 * i + 1] &= ~(0x0F << 26);

		/* program specific bit, for now */
		if (!tmp)
			tmp = 1;
		else
			tmp = 0;

		shader[4 * i + 1] |= tmp << 26;
	}
}

static void
vertex_shader_varyings_patch(unsigned int *shader, int size)
{
	int i;

	for (i = 0; i < size; i++) {
		int tmp;

		/* ignore entries for now, until we know more */

		tmp = (shader[4 * i + 2] >> 26) & 0x1F;

		if (tmp & 0x10) {
			tmp &= 0x0F;
			shader[4 * i + 2] &= ~(0x0F << 26);

			/* program specific bit, for now */
			if (!tmp)
				tmp = 1;
			else
				tmp = 0;

			shader[4 * i + 2] |= tmp << 26;
		}

		tmp = ((shader[4 * i + 2] >> 31) & 0x01) |
			((shader[4 * i + 3] << 1) & 0x1E);

		if (tmp & 0x10) {
			tmp &= 0x0F;
			shader[4 * i + 2] &= ~(0x01 << 31);
			shader[4 * i + 3] &= ~0x07;

			/* program specific bit, for now */
			if (!tmp)
				tmp = 1;
			else
				tmp = 0;

			shader[4 * i + 2] |= tmp << 31;
			shader[4 * i + 3] |= tmp >> 1;
		}
	}
}

void
premali_shader_binary_free(struct mali_shader_binary *binary)
{
	free(binary->error_log);
	free(binary->shader);
	free(binary->varying_stream);
	free(binary->uniform_stream);
	free(binary->attribute_stream);
	free(binary);
}

struct mali_shader_binary *
premali_shader_compile(int type, const char *source)
{
	struct mali_shader_binary *binary;
	int length = strlen(source);
	int ret;

	binary = calloc(1, sizeof(struct mali_shader_binary));
	if (!binary) {
		printf("%s: Error: allocation failed: %s\n",
		       __func__, strerror(errno));
		return NULL;
	}

	ret = __mali_compile_essl_shader(binary, type,
					 source, &length, 1);
	if (ret) {
		if (binary->error_log)
			printf("%s: compilation failed: %s\n",
			       __func__, binary->error_log);
		else
			printf("%s: compilation failed: %s\n",
			       __func__, binary->oom_log);

		premali_shader_binary_free(binary);
		return NULL;
	}

	return binary;
}

int
vertex_shader_attach(struct premali_state *state, const char *source)
{
	struct mali_shader_binary *binary;
	struct stream_uniform_table *uniform_table;
	struct stream_attribute_table *attribute_table;

	binary = premali_shader_compile(MALI_SHADER_VERTEX, source);
	if (!binary)
		return -1;

	uniform_table =
		stream_uniform_table_create(binary->uniform_stream,
					    binary->uniform_stream_size);
	if (uniform_table) {
		state->vertex_uniforms =
			stream_uniform_table_to_symbols(uniform_table,
							&state->vertex_uniform_count,
							&state->vertex_uniform_size);
		stream_uniform_table_destroy(uniform_table);
	}

	attribute_table =
		stream_attribute_table_create(binary->attribute_stream,
					      binary->attribute_stream_size);
	if (attribute_table) {
		state->vertex_attributes =
			stream_attribute_table_to_symbols(attribute_table,
							  &state->vertex_attribute_count);
		stream_attribute_table_destroy(attribute_table);

	}

	{
		unsigned int *shader = binary->shader;
		int size = binary->shader_size / 16;

		vertex_shader_attributes_patch(shader, size);
		vertex_shader_varyings_patch(shader, size);

		vs_info_attach_shader(state->vs, shader, size);
	}

	return 0;
}

int
fragment_shader_attach(struct premali_state *state, const char *source)
{
	struct mali_shader_binary *binary;
	struct stream_uniform_table *uniform_table;

	binary = premali_shader_compile(MALI_SHADER_FRAGMENT, source);
	if (!binary)
		return -1;

	uniform_table =
		stream_uniform_table_create(binary->uniform_stream,
					    binary->uniform_stream_size);
	if (uniform_table) {
		state->fragment_uniforms =
			stream_uniform_table_to_symbols(uniform_table,
							&state->fragment_uniform_count,
							&state->fragment_uniform_size);
		stream_uniform_table_destroy(uniform_table);
	}

	{
		unsigned int *shader = binary->shader;
		int size = binary->shader_size / 4;

		plbu_info_attach_shader(state->plbu, shader, size);
	}

	return 0;
}