/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2017 Phusion Holding B.V.
 *
 *  "Passenger", "Phusion Passenger" and "Union Station" are registered
 *  trademarks of Phusion Holding B.V.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_JSON_TOOLS_CBINDINGS_H_
#define _PASSENGER_JSON_TOOLS_CBINDINGS_H_

#include <stddef.h>

#ifdef __cplusplus
	extern "C" {
#endif


typedef enum {
	PSG_JSON_VALUE_TYPE_NULL,
	PSG_JSON_VALUE_TYPE_INT,
	PSG_JSON_VALUE_TYPE_UINT,
	PSG_JSON_VALUE_TYPE_REAL,
	PSG_JSON_VALUE_TYPE_STRING,
	PSG_JSON_VALUE_TYPE_BOOLEAN,
	PSG_JSON_VALUE_TYPE_ARRAY,
	PSG_JSON_VALUE_TYPE_OBJECT
} PsgJsonValueType;

typedef void PsgJsonValue;

PsgJsonValue *psg_json_value_new_null();
PsgJsonValue *psg_json_value_new_with_type(PsgJsonValueType type);
PsgJsonValue *psg_json_value_new_str(const char *val, size_t size);
PsgJsonValue *psg_json_value_new_int(int val);
PsgJsonValue *psg_json_value_new_uint(unsigned int val);
PsgJsonValue *psg_json_value_new_real(double val);
PsgJsonValue *psg_json_value_new_bool(int val);
void psg_json_value_free(PsgJsonValue *val);

PsgJsonValue *psg_json_value_set_value(PsgJsonValue *doc, const char *name, const PsgJsonValue *val);
PsgJsonValue *psg_json_value_set_str(PsgJsonValue *doc, const char *name, const char *val, size_t size);
PsgJsonValue *psg_json_value_set_int(PsgJsonValue *doc, const char *name, int val);
PsgJsonValue *psg_json_value_set_uint(PsgJsonValue *doc, const char *name, unsigned int val);
PsgJsonValue *psg_json_value_set_real(PsgJsonValue *doc, const char *name, double val);
PsgJsonValue *psg_json_value_set_bool(PsgJsonValue *doc, const char *name, int val);

PsgJsonValue *psg_json_value_append_val(PsgJsonValue *doc, const PsgJsonValue *val);

int psg_json_value_is_null(const PsgJsonValue *doc);
const PsgJsonValue *psg_json_value_get(const PsgJsonValue *doc, const char *name, size_t size);
const char *psg_json_value_as_cstr(const PsgJsonValue *doc);

PsgJsonValue *psg_autocast_value_to_json(const char *data, size_t size, char **error);


#ifdef __cplusplus
	} // extern "C"
#endif

#endif /* _PASSENGER_JSON_TOOLS_CBINDINGS_H_ */