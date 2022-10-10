
#define __STDC_WANT_LIB_EXT2__ 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h>

#include "json.h"
#include "MurmurHash3.h"

#define MURMUR_SEED 718281828


enum token_type {
	TOKEN_NONE = 0,
	TOKEN_ARRAY_START,
	TOKEN_ARRAY_END,
	TOKEN_OBJ_START,
	TOKEN_OBJ_END,
	TOKEN_STRING,
	TOKEN_NUMBER,
	TOKEN_TRUE,
	TOKEN_FALSE,
	TOKEN_NULL,
	TOKEN_INFINITY,
	TOKEN_UNDEFINED,
	TOKEN_NAN,
	TOKEN_LABEL,
	TOKEN_COMMA,
	TOKEN_COLON,
	TOKEN_COMMENT
};

struct json_obj_field {
	uint64_t hash;
	char* key;
	struct json_value* value;
};


struct token {
	enum token_type tokenType;
	struct json_value* val;
};


struct json_parser {
	int error;
	char* err_str;

	// lexing info
	char* source;
	char* end;
	size_t source_len;
	int eoi;
	int gotToken;
	
	char* head;
	int line_num; // these are 1-based
	int char_num;
	
	struct token cur_tok;
	
	// parsing info
	struct json_value** stack;
	int stack_cnt;
	int stack_alloc;
};


// sentinels for the parser stack

// the slot above contains an array, merge into it
struct json_value* RESUME_ARRAY = (struct json_value*)&RESUME_ARRAY;

// the slot above contains a label, the slot above that contains the object to merge into
struct json_value* RESUME_OBJ = (struct json_value*)&RESUME_OBJ;

// proper, safe bottom of the stack
struct json_value* ROOT_VALUE = (struct json_value*)&ROOT_VALUE;



static void dbg_print_token(struct token* ts);
static void dbg_dump_stack(struct json_parser* jp, int depth);




int json_array_push_tail(struct json_value* a, struct json_value* val) {

	struct json_link* node;
	
	node = malloc(sizeof(*node));
	if(!node) return 1;
	
	node->next = NULL;
	node->prev = a->arr.tail;
	node->v = val;
	
	if(a->len == 0) {
		a->arr.head = node;
	}
	else {
		a->arr.tail->next = node;
	}
	
	a->arr.tail = node;
	a->len++;
	
	return 0;
}

struct json_value* json_array_pop_tail(struct json_value* a) {

	struct json_value* v;
	struct json_link* t;
	
	if(a->len == 0) {
		return NULL;
	}
	
	a->len--;
	
	v = a->arr.tail->v;
	
	if(a->len > 0) {
		t = a->arr.tail;
		a->arr.tail = a->arr.tail->prev;
		a->arr.tail->next = NULL;
		
		free(t);
	}
	else {
		a->arr.head = a->arr.tail = NULL;
	}
	
	return v;
}


int json_array_push_head(struct json_value* a, struct json_value* val) {

	struct json_link* node;
	
	node = malloc(sizeof(*node));
	if(!node) return 1;
	
	node->prev = NULL;
	node->next = a->arr.head;
	node->v = val;
	
	if(a->len == 0) {
		a->arr.tail = node;
	}
	else {
		a->arr.head->next = node;
	}
	
	a->arr.head = node;
	a->len++;
	
	return 0;
}

// pops the tail
struct json_value* json_array_pop_head(struct json_value* a) {

	struct json_value* v;
	struct json_link* t;
	
	if(a->len == 0) {
		return NULL;
	}
	
	a->len--;
	
	v = a->arr.head->v;
	
	if(a->len > 0) {
		t = a->arr.head;
		a->arr.head = a->arr.head->prev;
		a->arr.head->next = NULL;
	
		free(t);
	}
	else {
		a->arr.tail = a->arr.head = NULL;
	}
	
	return v;
}


size_t json_array_calc_length(struct json_value* a) {
	size_t len;
	struct json_link* n;
	
	if(a->type != JSON_TYPE_ARRAY) {
		return 0;
	}
	
	len = 0;
	n = a->arr.head;
	while(n) {
		len++;
		n = n->next;
	}
	
	// update the cached value too
	a->len = len;
	
	return len;
}


// uses a truncated 128bit murmur3 hash
static uint64_t hash_key(char* key, intptr_t len) {
	uint64_t hash[2];
	
	// len is optional
	if(len == -1) len = strlen(key);
	
	MurmurHash3_x64_128(key, len, MURMUR_SEED, hash);
	
	return hash[0];
}

static int64_t find_bucket(struct json_value* obj, uint64_t hash, char* key) {
	int64_t startBucket, bi;
	
	bi = startBucket = hash % obj->obj.alloc_size; 
	
	do {
		struct json_obj_field* bucket;
		
		bucket = &obj->obj.buckets[bi];
		
		// empty bucket
		if(bucket->key == NULL) {
			return bi;
		}
		
		if(bucket->hash == hash) {
			if(!strcmp(key, bucket->key)) {
				// bucket is the right one and contains a value already
				return bi;
			}
			
			// collision, probe next bucket
		}
		
		bi = (bi + 1) % obj->obj.alloc_size;
	} while(bi != startBucket);
	
	printf("CJSON: error in find_bucket\n");
	// should never reach here if the table is maintained properly
	return -1;
}


// should always be called with a power of two
static int json_obj_resize(struct json_value* obj, int newSize) {
	struct json_obj_field* old, *op;
	size_t oldlen = obj->obj.alloc_size;
	int64_t n, bi;
	size_t i;
	
	old = op = obj->obj.buckets;
	
	obj->obj.alloc_size = newSize;
	obj->obj.buckets = calloc(1, sizeof(*obj->obj.buckets) * newSize);
	if(!obj->obj.buckets) return 1;
	
	for(i = 0, n = 0; i < oldlen && n < (int64_t)obj->len; i++) {
		if(op->key == NULL) {
			op++;
			continue;
		}
		
		bi = find_bucket(obj, op->hash, op->key);
		obj->obj.buckets[bi].value = op->value;
		obj->obj.buckets[bi].hash = op->hash;
		obj->obj.buckets[bi].key = op->key;
		
		n++;
		op++;
	}
	
	free(old);
	
	return 0;
}


// TODO: better return values and missing key handling
// returns 0 if val is set to the value
// *val == NULL && return > 0 means the key was not found;
int json_obj_get_key(struct json_value* obj, char* key, struct json_value** val) {
	uint64_t hash;
	int64_t bi;
	
	hash = hash_key(key, -1);
	
	bi = find_bucket(obj, hash, key);
	if(bi < 0 || obj->obj.buckets[bi].key == NULL) {
		*val = NULL;
		return 1;
	}
	
	*val = obj->obj.buckets[bi].value; 
	return 0;
}

// zero for success
int json_obj_set_key(struct json_value* obj, char* key, struct json_value* val) {
	uint64_t hash;
	int64_t bi;
	
	// check size and grow if necessary
	if((float)obj->len / (float)obj->obj.alloc_size >= 0.75) {
		json_obj_resize(obj, obj->obj.alloc_size * 2);
	}
	
	hash = hash_key(key, -1);
	
	bi = find_bucket(obj, hash, key);
	if(bi < 0) return 1;
	
	obj->obj.buckets[bi].value = val;
	obj->obj.buckets[bi].key = key;
	obj->obj.buckets[bi].hash = hash;
	obj->len++;
	
	return 0;
}



char* json_obj_get_strdup(struct json_value* obj, char* key) {
	char* s = json_obj_get_str(obj, key);
	return s ? strdup(s) : NULL;
}


// returns pointer to the internal string, or null if it's not a string
char* json_obj_get_str(struct json_value* obj, char* key) {
	json_value_t* val;
	
	if(json_obj_get_key(obj, key, &val)) {
		return NULL;
	}
	
	if(val->type != JSON_TYPE_STRING) {
		return NULL;
	}
	
	return val->s;
}

// returns an integer or the default value if it's not an integer
int64_t json_obj_get_int(struct json_value* obj, char* key, int64_t def) {
	json_value_t* val;
	
	if(json_obj_get_key(obj, key, &val)) {
		return def;
	}
	
	if(val->type != JSON_TYPE_INT) {
		return def;
	}
	
	return val->n;
}

// returns a double or the default value if it's not an integer
double json_obj_get_double(struct json_value* obj, char* key, double def) {
	json_value_t* val;
	
	if(json_obj_get_key(obj, key, &val)) {
		return def;
	}
	
	if(val->type != JSON_TYPE_DOUBLE) {
		return def;
	}
	
	return val->d;
}


// returns the json_value strut for a key, or null if it doesn't exist
struct json_value* json_obj_get_val(struct json_value* obj, char* key) {
	json_value_t* val;
	
	if(json_obj_get_key(obj, key, &val)) {
		return NULL;
	}

	return val;
}



// iteration. no order. results undefined if modified while iterating
// returns 0 when there is none left
// set iter to NULL to start
int json_obj_next(struct json_value* obj, void** iter, char** key, struct json_value** value) { 
	struct json_obj_field* b = *iter;
//	struct json_obj* obj;
	
	if(obj->type != JSON_TYPE_OBJ) return 1;
	
	
	if(obj->len == 0) {
		return 0;
	}
	
	// a tiny bit of idiot-proofing
	if(b == NULL) b = &obj->obj.buckets[-1];
	//printf("alloc size: %d\n", obj->alloc_size);
	do {
		b++;
		if(b >= obj->obj.buckets + obj->obj.alloc_size) {
			//printf("ending next\n");
			// end of the list
			*value = NULL;
			*key = NULL;
			return 0;
		}
	} while(!b->key);
	
	*key = b->key;
	*value = b->value;
	*iter = b;
	
	return 1;
}


/* key, target, offset, type */
// returns number of values filled
int json_obj_unpack_struct(int count, struct json_value* obj, ...) {
	int i, ret, filled;
	char* key;
	void* target;
	size_t offset;
	enum json_type type;
	struct json_value* v;
	va_list ap;
	
	
	if(obj->type != JSON_TYPE_OBJ) return 0; 

	filled = 0;
	
	va_start(ap, obj);
	for(i = 0; i < count; i++) {
		key = va_arg(ap, char*); 
		if(!key) break;
		target = va_arg(ap, void*); 
		offset = va_arg(ap, size_t); 
		type = va_arg(ap, enum json_type); 
		
		ret = json_obj_get_key(obj, key, &v);
		if(ret > 0) continue;
		
		if(!json_as_type(v, type, target + offset)) filled++;
	}
	va_end(ap);
	
	return filled;
}

/*
returns an array of char* pairs, key then value

int json_obj_unpack_string_array(struct json_value* obj, char*** out, size_t* len) {
	char** a;
	size_t l, i;
	
	int ret;
	struct json_obj* o;
	void* iter;
	char* key;
	struct json_value* v;
	

	
	
	// TODO: error handling
	if(obj->type != JSON_TYPE_OBJ) {
		return 1;
	}
	o = obj->v.obj;
	
	l = o->len;
	if(l <= 0) {
		return 2;
	}
	
	a = malloc(l * 2 * sizeof(*a));
	if(!a) {
		return 3;
	}
	
	// do shit
	i = 0;
	iter = NULL;
	while(json_obj_next(obj, &iter, &key, &v)) {
		a[i++] = key; // todo: strdup?
		ret = json_as_string(v, &a[i++]);
		// TODO: handle errors here
	}
	
	*out = a;
	*len = l;
	
	return 0;
}
*/


/*
type coercion rules:
undefined/null -> int = 0
numbers, as you would expect, according to C type conversion rules
obj/array -> number/string = error
string/comment -> string = string
number -> string = sprintf, accoding to some nice rules.
string -> number = strtod/i

strings are dup'd

JSON_TYPE_INT is assumed to be C int

numbers over 2^63 are not properly supported yet. they will be truncated to 0
*/

// returns 0 if successful
int json_as_type(struct json_value* v, enum json_type t, void* out) { 
	int ret;
	int64_t i;
	double d;
	char* s;
	
	if(!v) return 1;
	
	switch(t) { // actual type

		case JSON_TYPE_INT:
			i = json_as_int(v);
			*((int*)out) = i;
			return 0;
			
		case JSON_TYPE_DOUBLE: 
			d = json_as_double(v);
			*((double*)out) = d;
			return 0;
			
		case JSON_TYPE_STRING:
			s = json_as_strdup(v);
			*((char**)out) = s;
			return 0;
		
		case JSON_TYPE_OBJ: 
		case JSON_TYPE_ARRAY: 
			*((struct json_value**)out) = v; 
			return 0;

			
		case JSON_TYPE_FLOAT:
			d = json_as_double(v);
			*((float*)out) = d;
			return 0;
			
		case JSON_TYPE_INT8:
			i = json_as_int(v);
			*((int8_t*)out) = i;
			return 0;
			
		case JSON_TYPE_INT16:
			i = json_as_int(v);
			*((int16_t*)out) = i;
			return 0;
			
		case JSON_TYPE_INT32: 
			i = json_as_int(v);
			*((int32_t*)out) = i;
			return 0;
			
		case JSON_TYPE_INT64: 
			i = json_as_int(v);
			*((int64_t*)out) = i;
			return 0;
			
		case JSON_TYPE_UINT8:
			i = json_as_int(v);
			*((uint8_t*)out) = i;
			return 0;
			
		case JSON_TYPE_UINT16:
			i = json_as_int(v);
			*((uint16_t*)out) = i;
			return 0;
			
		case JSON_TYPE_UINT32: 
			i = json_as_int(v);
			*((uint32_t*)out) = i;
			return 0;
			
		case JSON_TYPE_UINT64: 
			i = json_as_int(v);
			*((uint64_t*)out) = i;
			return 0;
			
		case JSON_TYPE_UNDEFINED:
		case JSON_TYPE_NULL:
		case JSON_TYPE_COMMENT_SINGLE:
		case JSON_TYPE_COMMENT_MULTI:
		default:
			return 1;
	}
	
	
	
}

// returns 0 for success
int64_t json_as_int(struct json_value* v) {
	switch(v->type) { // actual type
		case JSON_TYPE_UNDEFINED:
		case JSON_TYPE_NULL:
			return 0;
			
		case JSON_TYPE_INT:
			return v->n;
			
		case JSON_TYPE_DOUBLE:
			return v->d;
			
		case JSON_TYPE_STRING:
			return strtol(v->s, NULL, 0);
			
		case JSON_TYPE_OBJ:
		case JSON_TYPE_ARRAY:
		case JSON_TYPE_COMMENT_SINGLE:
		case JSON_TYPE_COMMENT_MULTI:
		default:
			return 0;
	}
}

// returns 0 for success
double json_as_double(struct json_value* v) {
	switch(v->type) { // actual type
		case JSON_TYPE_UNDEFINED:
		case JSON_TYPE_NULL:
			return 0.0;
			
		case JSON_TYPE_INT:
			return v->n;
			
		case JSON_TYPE_DOUBLE:
			return v->d;
			
		case JSON_TYPE_STRING:
			return strtod(v->s, NULL);
			
		case JSON_TYPE_OBJ:
		case JSON_TYPE_ARRAY:
		case JSON_TYPE_COMMENT_SINGLE:
		case JSON_TYPE_COMMENT_MULTI:
		default:
			return 0.0;
	}
}


static char* a_sprintf(char* fmt, ...) {
	va_list args;
	char* buf;
	size_t len;
	int n;

	va_start(args, fmt);

	len = vsnprintf(NULL, 0, fmt, args);
	buf = malloc(len + 1);
	if(!buf) return NULL;

	vsnprintf(buf, len, fmt, args);

	va_end (args);

	return buf;
}

// returns 0 for success
char* json_as_strdup(struct json_value* v) {
	char* buf;
	size_t len;
	
	switch(v->type) { // actual type
		case JSON_TYPE_UNDEFINED:
			return strdup("undefined");
			
		case JSON_TYPE_NULL:
			return strdup("null");
			
		case JSON_TYPE_INT:
			return a_sprintf("%ld", v->n); // BUG might leak memory
			
		case JSON_TYPE_DOUBLE:
			return a_sprintf("%f", v->d);
			
		case JSON_TYPE_COMMENT_SINGLE:
		case JSON_TYPE_COMMENT_MULTI:
		case JSON_TYPE_STRING:
			return strdup(v->s);
			
		case JSON_TYPE_OBJ:
			return strdup("[Object]");
			
		case JSON_TYPE_ARRAY:
			return strdup("[Array]");
		
		default:
			return strdup("");
	}
}


float json_as_float(struct json_value* v) {
	return json_as_double(v); 
}



// out must be big enough, at least as big as in+1 just to be safe
// appends a null to out, but is also null-safe
static int decode_c_escape_str(char* in, char* out, size_t len, size_t* outLen) {
	size_t i, o;
	
	char tmp[7];
	
	for(i = 0, o = 0; i < len; i++, o++) {
		if(*in == '\\') {
			in++;
			i++;
			switch(*in) {
				case '0': *out = '\0'; break; 
				case 'r': *out = '\r'; break; 
				case 'n': *out = '\n'; break;
				case 'f': *out = '\f'; break;
				case 'a': *out = '\a'; break;
				case 'b': *out = '\b'; break;
				case 'v': *out = '\v'; break;
				case 't': *out = '\t'; break;
				case 'x': 
				
					if(len < i + 1) {
//						jp->error = JSON_PARSER_ERROR_UNEXPECTED_EOI;
						//printf("JSON: EOF in hex escape sequence\n");
						return 1;
					}
					if(!isxdigit(in[1])) {
						// malformed hex code. output an 'x' and keep going.
						*out = 'x';
						break;
					}
					
					tmp[0] = in[1];	
				
				
					if(len < i + 2) {
//						jp->error = JSON_PARSER_ERROR_UNEXPECTED_EOI;
						//printf("JSON: EOF in hex escape sequence\n");
						return 1;
					}
					if(!isxdigit(in[2])) {
						// malformed hex code, but we have one digit
						tmp[1] = 0;
						
						*out = strtol(tmp, NULL, 16);
						in++; i++;
						break;
					}
					else {
						tmp[1] = in[2];
						tmp[2] = 0;
						
						*out = strtol(tmp, NULL, 16);
						in += 2; i += 2;
					}					
					break;
	
				case 'u': 
					if(in[1] == '{') {
						int n;
						int32_t code;
						char* s;
						// seek forward to the closing '}'
						for(n = 0, s = in + 2;; n++) {
							if(i + 3 >= len) {
								//printf("JSON: EOF in unicode escape sequence\n");
								return 2;
							}
							
							if(s[0] == '}') break;
							
							if(n == INT_MAX) {
								//printf("JSON: malformed unicode escape sequence\n");
								return 3;
							}
							
							if(!isxdigit(s[0])) {
								//printf("JSON: invalid character inside unicode escape sequence\n");
								break;
							}
							
							s++;
						}
						
						int nl = n > 6 ? 6 : n;
					
						if(n > 0) {
							strncpy(tmp, s - nl, nl);
							tmp[nl] = 0;
							code = strtol(tmp, NULL, 16);
						}
						else {
							code = 0;
						}
						
						*out = code; // todo: utf8 conversion
						in += n + 2; i += n + 2;
					}
					else {
						int n;
						int32_t code;
						char* s;
						// seek forward to the closing '}'
						for(n = 0, s = in + 1; n < 4; n++) {
							if(i + 2 >= len) {
								//printf("JSON: EOF in unicode escape sequence\n");
								return 2;
							}
							
							if(!isxdigit(s[0])) {
								break;
							}
							
							s++;
						}
					
						if(n > 0) {
							strncpy(tmp, in + 1, n);
							tmp[n] = 0;
							code = strtol(tmp, NULL, 16);
						}
						else {
							code = 0;
						}
						
						*out = code; // todo: utf8 conversion
						in += n; i += n;
					}
					
					break;
					
				default:
					// pass-through
					*out = in[0];
			}
		}
		else {
			*out = *in;
		}
		
		out++;
		in++;
	}
	
	*out = '\0';
	if(outLen) *outLen = o; 
	
	return 0;
}




///////////////////
//    Parser     //
///////////////////



// move forward one char
static void lex_next_char(struct json_parser* jl) {
	if(jl->error) { 
		//printf("JSON: next char has error\n");
		return;
	}
	if(*jl->head == '\n') {
		jl->line_num++;
		jl->char_num = 1;
	}
	else {
		jl->char_num++;
	}
	
	jl->head++;
	//printf("%c\n", *jl->head);
	//printf("line/char [%d/%d]\n", jl->line_num, jl->char_num);
}



// returns erro code.
static int lex_push_token_val(struct json_parser* jp, enum token_type t, struct json_value* val) {
	
	jp->gotToken = 1;
	jp->cur_tok.tokenType = t;
	jp->cur_tok.val = val;
	
	return 0;
}



// returns error code. val set to null
static int lex_push_token(struct json_parser* jl, enum token_type t) {
	return lex_push_token_val(jl, t, NULL);
}


static int lex_string_token(struct json_parser* jl) {
	size_t len;
	struct json_value* val;
	char* str;
	char delim = *jl->head;
	char* se = jl->head + 1;
	
	int lines = 0;
	int char_num = jl->char_num;
	

	// find len, count lines
	while(1) {
		
		if(*se == delim && *(se-1) != '\\') break;
		if(*se == '\0') {
			jl->error = JSON_LEX_ERROR_NULL_IN_STRING;
			return 1;
		}
		
		if(*se == '\n') {
			lines++;
			char_num = 1;
		}
		char_num++;
		se++;
	//	printf("%d.%d\n", jl->line_num + lines, char_num);
		if(se > jl->end) {
			jl->error = JSON_LEX_ERROR_UNEXPECTED_END_OF_INPUT;
			return 1;
		}
	}
	
	len = se - jl->head - 1;
	
	str = malloc(len+1);
	
	if(decode_c_escape_str(jl->head + 1, str, len, NULL)) {
		jl->error = JSON_LEX_ERROR_INVALID_STRING;
		return 1;
	}
	
	// json value
	val = calloc(1, sizeof(*val));
	val->type = JSON_TYPE_STRING;
	val->s = str;
	
	lex_push_token_val(jl, TOKEN_STRING, val);
	
	// advance to the end of the string
	jl->head = se;
	jl->char_num = char_num;
	jl->line_num += lines;
	
	
	return 0;
}


static int lex_number_token(struct json_parser* jl) {
	char* start, *s, *e;
	int is_float = 0;
	int negate =0;
	int base;
	
	start = jl->head;
	
	if(*start == '-') negate = 1;
	if(*start == '+' || *start == '-') start++;
	
	s = start;
	
	// check if it's a float
	while(s < jl->end) {
		if(*s == '.' || *s == 'e' || *s == 'E')
			is_float = 1;
		
		if(*s < '0' || *s > '9') break;
		
		s++;
	} 
	
	s = start;
	
	struct json_value* val;
	val = malloc(sizeof(*val));
	
	// read the value
	if(is_float) {
		base = -1;
		val->d = strtod(s, &e);
		val->type = JSON_TYPE_DOUBLE;
		if(negate) val->d *= -1;
	}
	else {
		if(*s == '0') {
			if(s[1] == 'x') { // hex
				base = 16;
				s += 2;
			}
			else if(s[1] == 'b') { //binary
				base = 2;
				s += 2;
			}
			else base = 8;
		}
		else base = 10;
		
		val->n = strtol(s, &e, base);
		val->type = JSON_TYPE_INT;
		if(negate) val->n *= -1;
	}
	
	val->base = base;
	
	lex_push_token_val(jl, TOKEN_NUMBER, val);
	
	// advance to the end of the string
	jl->char_num += e - jl->head - 1;
	jl->head = e - 1;
	
//	printf("head %c\n", *jl->head);
//	printf("lc/line/char [%d/%d/%d]\n", jl->head - jl->source, jl->line_num, jl->char_num);
	
	return 0;
}


static int lex_label_token(struct json_parser* jl) {
	size_t len;
	struct json_value* val;
	char* str;
	char* se = jl->head;
	
	int char_num = jl->char_num;

	// check for boolean literals
	// TODO: proper ident char checking
	if(0 == strncasecmp(se, "true", strlen("true"))) {
		if(!isalnum(se[strlen("true")]) && se[strlen("true")] != '_') {
			lex_push_token_val(jl, TOKEN_TRUE, json_new_strn(se, strlen("true")));
			jl->head += strlen("true");
			return 0;
		}
	}
	if(0 == strncasecmp(se, "false", strlen("false"))) {
		if(!isalnum(se[strlen("false")]) && se[strlen("true")] != '_') {
			lex_push_token_val(jl, TOKEN_FALSE, json_new_strn(se, strlen("false")));
			jl->head += strlen("false");
			return 0;
		}
	}
	if(0 == strncasecmp(se, "null", strlen("null"))) {
		if(!isalnum(se[strlen("null")]) && se[strlen("null")] != '_') {
			lex_push_token_val(jl, TOKEN_FALSE, json_new_strn(se, strlen("null")));
			jl->head += strlen("null");
			return 0;
		}
	}
	if(0 == strncasecmp(se, "undefined", strlen("undefined"))) {
		if(!isalnum(se[strlen("undefined")]) && se[strlen("undefined")] != '_') {
			lex_push_token_val(jl, TOKEN_FALSE, json_new_strn(se, strlen("undefined")));
			jl->head += strlen("undefined");
			return 0;
		}
	}
	
	//printf("error: %d\n", jl->error);
	// find len, count lines
	while(1) {
		if(!((*se >= 'a' && *se <= 'z')
			|| (*se >= 'A' && *se <= 'Z')
			|| (*se >= '0' && *se <= '9')
			|| *se == '_' || *se == '$')
		) break;
		
		char_num++;
		se++;
		//printf("%d.%d\n", jl->line_num, char_num);
		if(se > jl->end) {
			jl->error = JSON_LEX_ERROR_UNEXPECTED_END_OF_INPUT;
			return 1;
		}
	}
	
	len = se - jl->head;
	// TODO: check for null, infinity, undefined, nan
	
	str = malloc(len+1);
	strncpy(str, jl->head, len);
	str[len] = 0;
	
	// json value
	val = calloc(1, sizeof(*val));
	val->type = JSON_TYPE_STRING;
	val->s = str;
	
	lex_push_token_val(jl, TOKEN_LABEL, val);
	
	// advance to the end of the string
	jl->head = se - 1;
	jl->char_num = char_num - 1;
	
	//printf("head %c\n", *jl->head);
	//printf("lc/line/char [%d/%d/%d]\n", jl->head - jl->source, jl->line_num, jl->char_num);
	
	return 0;
}



static int lex_comment_token(struct json_parser* jl) {
	char* start, *se, *str;
	char delim;
	size_t len;
	int lines, char_num;
	struct json_value* val;
	
	lex_next_char(jl);
	
	start = se = jl->head + 1; 
	delim = *jl->head;
	
	lines = 0;
	char_num = jl->char_num;
	
	if(delim == '/') { // single line comment
		// look for a linebreak;
		while(1) {
			if(se[0] == '\n') break;
			if(*se == '\0') {
				jl->error = JSON_LEX_ERROR_NULL_BYTE;
				return 1;
			}
			
			char_num++;
			se++;
			
			if(se > jl->end) {
				jl->error = JSON_LEX_ERROR_UNEXPECTED_END_OF_INPUT;
				return 1;
			}
		}
	}
	else if(delim == '*') { // multline
		// find len, count lines
		while(1) {
			if(se[0] == '*' && se[1] == '/') break;
			if(*se == '\0') {
				jl->error = JSON_LEX_ERROR_NULL_BYTE;
				return 1;
			}
			
			if(*se == '\n') {
				lines++;
				char_num = 1;
			}
			char_num++;
			se++;
			
			if(se > jl->end) {
				jl->error = JSON_LEX_ERROR_UNEXPECTED_END_OF_INPUT;
				return 1;
			}
		}
	}
	else {
		//printf("JSON: broken comment\n");
		jl->error = JSON_LEX_ERROR_INVALID_CHAR;
		return 1;
	}
	
	len = se - jl->head - 1;
	
#if JSON_DISCARD_COMMENTS == 0
	
	str = malloc(len+1);
	check_oom(str)
	
//	printf("error: %d\n", jl->error);
	
	strncpy(str, start, len);
	
	
//printf("error: %d\n", jl->error);
	// json value
	val = calloc(1, sizeof(*val));
	check_oom(val)
//	printf("error: %d\n", jl->error);
	val->type = delim == '*' ? JSON_TYPE_COMMENT_MULTI : JSON_TYPE_COMMENT_SINGLE;
	val->v.str = str;
	
	lex_push_token_val(jl, TOKEN_COMMENT, val);
	
#endif // JSON_DISCARD_COMMENTS
	
	// advance to the end of the string
	jl->head = se;
	jl->char_num = char_num;
	jl->line_num += lines;

	if(delim == '*') lex_next_char(jl);
	
//	printf("--head %c\n", *jl->head);
//	printf("lc/line/char [%d/%d/%d]\n", jl->head - jl->source, jl->line_num, jl->char_num);
	
	return 0;
}

// returns false when there is no more input

//       formerly lex_nibble()
static int lex_next_token(struct json_parser* jl) {
	
	
	jl->gotToken = 0;
	
	while(!jl->gotToken) {
		char c = *jl->head;
	
		switch(c) {
			case '{': lex_push_token(jl, TOKEN_OBJ_START); break;
			case '}': lex_push_token(jl, TOKEN_OBJ_END); break;
			case '[': lex_push_token(jl, TOKEN_ARRAY_START); break;
			case ']': lex_push_token(jl, TOKEN_ARRAY_END); break;
			case ',': lex_push_token(jl, TOKEN_COMMA); break;
			case ':': lex_push_token(jl, TOKEN_COLON); break;
			
			case '/': lex_comment_token(jl); break;
			
			case '\'':
			case '"':
			case '`':
				lex_string_token(jl);
				break;
			
			case '0': case '1': case '2': case '3': case '4': 
			case '5': case '6': case '7': case '8': case '9':
			case '-': case '+': case '.':
				lex_number_token(jl);
				break;
			
			case ' ':
			case '\t':
			case '\r':
			case '\f':
			case '\v':
			case '\n':
				break;
				
			default:
				if(isalpha(c) || c == '_' || c == '$') {
					lex_label_token(jl);
					break;
				}
				
				if(c == 0) {
					return 1; // end of file
				}
				
				// lex error
				jl->error = JSON_LEX_ERROR_INVALID_CHAR;
				return 1;
		}
		
		//printf("lol\n");
		
		lex_next_char(jl);
	}
	
	//printf("token %d:%d ", jl->line_num, jl->char_num);
	dbg_print_token(&jl->cur_tok);
	
	jl->eoi = jl->head >= jl->end;
	
	if(jl->error) return jl->error;
	return jl->eoi;
}

static int parser_indent_level = 0;
static void dbg_parser_indent(void) { return;
	int i;
	for(i = 0; i < parser_indent_level; i++) {
		dbg_printf("    ");
	}
}


static void parser_push(struct json_parser* jp, struct json_value* v) {
	void* tmp;
	
	int alloc = jp->stack_alloc;
	int cnt = jp->stack_cnt;
	
	// check size
	if(cnt >= alloc) {
		if(alloc == 0) alloc = 16;
		alloc *= 2;
		tmp = realloc(jp->stack, alloc * sizeof(*(jp->stack)));
		if(!tmp) {
			jp->error = JSON_ERROR_OOM;
			return;
		}
		
		jp->stack = tmp;
		jp->stack_alloc = alloc;
	}
	
	jp->stack[cnt] = v;
	jp->stack_cnt++;
}

static struct json_value* parser_pop(struct json_parser* jp) {
	
	if(jp->stack_cnt <= 0) {
		jp->error = JSON_PARSER_ERROR_STACK_EXHAUSTED;
		return NULL;
	}
	
	return jp->stack[--jp->stack_cnt];
}

static void parser_push_new_array(struct json_parser* jp) {
	
	struct json_value* val;
	
	val = malloc(sizeof(*val));
	if(!val) {
		jp->error = JSON_ERROR_OOM;
		return;
	}
	
	val->type = JSON_TYPE_ARRAY;
	val->len = 0;
	val->base = 0;
	val->arr.head = NULL;
	val->arr.tail = NULL;
	
	parser_push(jp, val);
}

static void parser_push_new_object(struct json_parser* jp) {
	
	struct json_value* val;
	
	val = json_new_object(8);
	if(!val) {
		jp->error = JSON_ERROR_OOM;
		return;
	}
	
	
// 	printf("vt: %d\n", val);
	dbg_dump_stack(jp, 3);
	parser_push(jp, val);
	dbg_dump_stack(jp, 4);
}


// not used atm. comments will probably be moved to a side channel
static void consume_comments(struct json_parser* jp) {
	while(1) {
		if(jp->cur_tok.tokenType != TOKEN_COMMENT) break;
		
		parser_push(jp, jp->cur_tok.val);
		lex_next_token(jp);
	}
}

static void consume_commas(struct json_parser* jp) {
//	lex_next_token(jp);
	while(jp->cur_tok.tokenType == TOKEN_COMMA) lex_next_token(jp);
}

static void reduce_array(struct json_parser* jp) {
	/* what the stack should look like now
	  ...
	1 array
	0 any value
	
	*/
	
	if(jp->stack_cnt < 2) {
		dbg_printf("stack too short in reduce_array: %d \n", jp->stack_cnt);
		jp->error = JSON_PARSER_ERROR_STACK_EXHAUSTED;
		return;
	}
	
	struct json_value** st = jp->stack + jp->stack_cnt - 1; 
	struct json_value* v = st[0];
	struct json_value* arr = st[-1];
	
	if(arr == ROOT_VALUE) return;
	
	if(arr->type != JSON_TYPE_ARRAY) {
		jp->error = JSON_PARSER_ERROR_CORRUPT_STACK;
		return;
	}
	// append v to arr
	json_array_push_tail(arr, v);
	
	jp->stack_cnt--;
}

static void reduce_object(struct json_parser* jp) {
	/* what the stack should look like now
	  ...
	2 object
	1 label
	0 any value
	
	*/
	if(jp->stack_cnt < 3) {
		jp->error = JSON_PARSER_ERROR_STACK_EXHAUSTED;
		return;
	}
	
	struct json_value** st = jp->stack + jp->stack_cnt - 1; 
	struct json_value* v = st[0];
	struct json_value* l = st[-1];
	struct json_value* obj = st[-2];
	
	if(obj == ROOT_VALUE) return;
	
	// TODO: check label type
	dbg_dump_stack(jp, 10);
	if(obj->type != JSON_TYPE_OBJ) {// printf("invalid obj\n");
		
		
		dbg_printf("0 type: %d \n", v->type);
		dbg_printf("1 type: %d \n", l->type);
		dbg_printf("2 type: %d \n", obj->type);
		dbg_printf("3 type: %d \n", st[-3]->type);
		
		jp->error = JSON_PARSER_ERROR_CORRUPT_STACK;
		return;
	}
	
	// insert l:v into obj
	json_obj_set_key(obj, l->s, v);
	// BUG? free label value?
	
	jp->stack_cnt -= 2;
}


static struct json_parser* parse_token_stream(char* source, size_t len) {
	
	int i;
	struct json_parser* jp;
	
	jp = calloc(1, sizeof(*jp));
	if(!jp) {
		return NULL;
	}
	
	jp->source = source;
	jp->end = source + len;
	jp->source_len = len;
	
	jp->head = source;
	jp->line_num = 1; // these are 1-based
	jp->char_num = 1;
	
	i = 0;

#define next() lex_next_token(jp); //) goto UNEXPECTED_EOI;
#define is_end() if(jp->eoi) goto UNEXPECTED_EOI;

	// the root value sentinel helps a few algorithms and marks a proper end of input
	parser_push(jp, ROOT_VALUE);
	
	// get the first token
	lex_next_token(jp);
	if(jp->cur_tok.tokenType == TOKEN_OBJ_START) {
		parser_push_new_object(jp);
		next();
		goto PARSE_OBJ;
	}
	
	PARSE_ARRAY: // not actually starting with an array; this is just the type probing code
		dbg_parser_indent();
		dbg_printf("\nparse_array l:%d, c:%d \n", jp->line_num, jp->char_num);
		
		
		if(jp->eoi) goto CHECK_END;
	
		// cycle: val, comma
		switch(jp->cur_tok.tokenType) {
		
			case TOKEN_ARRAY_START: dbg_parser_indent();dbg_printf("TOKEN_ARRAY_START\n");
				parser_indent_level++;
				parser_push(jp, RESUME_ARRAY);
				parser_push_new_array(jp);
				next();
				goto PARSE_ARRAY;
				
			case TOKEN_ARRAY_END: dbg_parser_indent();dbg_printf("TOKEN_ARRAY_END\n");
			parser_indent_level--;
				/* what the stack should look like now
					...
					3 parent container 
					2 ? possibly a label ?
					1 -- resume sentinel --
					0 array to be closed
				*/
				{
					// TODO check stack depth
					struct json_value* closed = parser_pop(jp);
					struct json_value* sentinel = parser_pop(jp);
					
					// put the closed array back on the stack then reduce it appropriately
					parser_push(jp, closed);
					
					if(sentinel == RESUME_ARRAY) {
						reduce_array(jp);
						
//						consume_commas(jp);
						next();
						goto PARSE_ARRAY;
					}
					else if(sentinel == RESUME_OBJ) {
						reduce_object(jp);
						
//						consume_commas(jp);
						next();
						goto PARSE_OBJ;
					}
					else if(sentinel == ROOT_VALUE) {
						// proper finish
						goto END;
					}
					else {
						goto INVALID_SENTINEL;
					}
				}
//				consume_commas(jp);
//				next();
				break;
			
			case TOKEN_OBJ_START: dbg_parser_indent();dbg_printf("PARSE_OBJ\n");
				parser_indent_level++;
				parser_push(jp, RESUME_ARRAY);
				parser_push_new_object(jp);
				next();
				goto PARSE_OBJ;
				
			case TOKEN_STRING:
			case TOKEN_NUMBER:
			case TOKEN_NULL:
			case TOKEN_UNDEFINED: dbg_parser_indent();dbg_printf("PARSE VALUE \n");
				parser_push(jp, jp->cur_tok.val);
				reduce_array(jp);
				
//				consume_commas(jp);
				next();
				
				goto PARSE_ARRAY;
		
			case TOKEN_OBJ_END: dbg_parser_indent();dbg_printf("BRACKET_MISMATCH\n");
				goto BRACKET_MISMATCH;
			
			case TOKEN_COMMA:
				next();
				goto PARSE_ARRAY;
			
			case TOKEN_NONE:
			case TOKEN_LABEL:
			case TOKEN_COLON: 
			default: dbg_printf("UNEXPECTED_TOKEN\n");
				// invalid
				goto UNEXPECTED_TOKEN;
		}
	
		return NULL;
	
	PARSE_OBJ:
		dbg_parser_indent();dbg_printf("\nparse_obj l:%d, c:%d \n", jp->line_num, jp->char_num);
	// cycle: label, colon, val, comma
		consume_commas(jp);
	
		if(jp->cur_tok.tokenType == TOKEN_OBJ_END) {
			dbg_parser_indent();dbg_printf("obj- TOKEN_OBJ_END\n");
			parser_indent_level--;
			/* what the stack should look like now
				...
				3 parent container 
				2 ? possibly a label ?
				1 -- resume sentinel --
				0 object to be closed
			*/
			{
				// TODO check stack depth
				struct json_value* closed = parser_pop(jp);
				struct json_value* sentinel = parser_pop(jp);
				
				// put the closed array back on the stack then reduce it appropriately
				parser_push(jp, closed);
				if(sentinel == RESUME_ARRAY) {
					reduce_array(jp);
					
					next();
//					consume_commas(jp);
					goto PARSE_ARRAY;
				}
				else if(sentinel == RESUME_OBJ) {
					reduce_object(jp);
					
					next();
//					consume_commas(jp);
					goto PARSE_OBJ;
				}
				else if(sentinel == ROOT_VALUE) {
					// proper finish
					goto END;
				}
				else {
					goto INVALID_SENTINEL;
				}
			}
			
			dbg_printf("ending obj in begining of obj\n");
			goto UNEXPECTED_TOKEN;
		}
		
		
		switch(jp->cur_tok.tokenType) {
			case TOKEN_LABEL:
			case TOKEN_STRING:
			case TOKEN_TRUE:
			case TOKEN_FALSE:
			case TOKEN_INFINITY:
			case TOKEN_NULL:
			case TOKEN_UNDEFINED:
			case TOKEN_NAN:
				parser_push(jp, jp->cur_tok.val);
				break;
			
			default:
				// error
				dbg_printf("!!!missing label\n");
				goto UNEXPECTED_TOKEN;
		}
		dbg_dump_stack(jp, 5);
		next();
		
		if(jp->cur_tok.tokenType != TOKEN_COLON) {
			// error
			dbg_printf("!!!missing colon\n");
			goto UNEXPECTED_TOKEN;
		}
		next();

	
		switch(jp->cur_tok.tokenType) {
		
			case TOKEN_ARRAY_START: dbg_parser_indent();dbg_printf("obj- TOKEN_ARRAY_START\n");
				parser_indent_level++;
				parser_push(jp, RESUME_OBJ);
				parser_push_new_array(jp);
				next();
				goto PARSE_ARRAY;
				
			case TOKEN_OBJ_END: dbg_parser_indent();dbg_printf("obj- !!!TOKEN_OBJ_END in value slot\n");

				
				 dbg_printf("!!! escaped sentinel block\n");
				break;
			
			case TOKEN_OBJ_START: dbg_parser_indent();dbg_printf("obj- TOKEN_OBJ_START\n");
				parser_indent_level++;
				parser_push(jp, RESUME_OBJ);
				parser_push_new_object(jp); // BUG
				next();
				goto PARSE_OBJ;
				
			case TOKEN_STRING:
			case TOKEN_NUMBER:
			case TOKEN_NULL:
			case TOKEN_UNDEFINED: dbg_parser_indent();dbg_printf("obj- TOKEN VALUE\n");
				parser_push(jp, jp->cur_tok.val);
				reduce_object(jp);
				
				next();
				consume_commas(jp);
				
				goto PARSE_OBJ;
		
			case TOKEN_ARRAY_END: dbg_parser_indent();dbg_printf("obj- BRACE_MISMATCH\n");
				goto BRACE_MISMATCH;

			case TOKEN_COMMA: dbg_parser_indent();dbg_printf("obj- eating comma\n");
				next();
				goto PARSE_OBJ;
							
			case TOKEN_NONE:
			case TOKEN_LABEL:
			case TOKEN_COLON:
			default: dbg_printf("obj- UNEXPECTED_TOKEN\n");
				// invalid
				goto UNEXPECTED_TOKEN;
		}
	
	dbg_parser_indent();dbg_printf("end of obj\n");

	return NULL;
	
CHECK_END:  dbg_parser_indent();dbg_printf("!!! CHECK_END\n");
	if(jp->stack_cnt == 2) {
		// check root value sentinel
		// good, fall through to END
	}
	else if(jp->stack_cnt == 1) {
		// the file is empty
		goto UNEXPECTED_EOI;
	}
	else {
		// stuff is left on the stack
		goto UNEXPECTED_EOI;
	}
	//i = *((int*)0);
END:  dbg_printf("!!! END\n");
	if(jp->error) dbg_printf("parsing error: %d\n", jp->error);
	return jp;
UNEXPECTED_EOI: // end of input
	dbg_printf("!!! UNEXPECTED_EOI\n");
	jp->error = JSON_PARSER_ERROR_UNEXPECTED_EOI;
	return jp;
UNEXPECTED_TOKEN: dbg_printf("!!! UNEXPECTED_TOKEN\n");
	jp->error = JSON_PARSER_ERROR_UNEXPECTED_TOKEN;
	return jp;
BRACE_MISMATCH: dbg_printf("!!! BRACE_MISMATCH\n");
	jp->error = JSON_PARSER_ERROR_BRACE_MISMATCH;
	return jp;
BRACKET_MISMATCH: dbg_printf("!!! BRACKET_MISMATCH\n");
	jp->error = JSON_PARSER_ERROR_BRACKET_MISMATCH;
	return jp;
INVALID_SENTINEL: dbg_printf("!!! INVALID_SENTINEL\n");
	jp->error = JSON_PARSER_ERROR_CORRUPT_STACK;
	return jp;
}


struct json_file* json_load_path(char* path) {
	struct json_file* jf;
	FILE* f;
	
	f = fopen(path, "rb");
	if(!f) {
		//fprintf(stderr, "JSON: no such file: \"%s\"\n", path);
		return NULL;
	}
	
	jf = json_read_file(f);
	
	fclose(f);
	
	return jf;
}

struct json_file* json_read_file(FILE* f) {
	size_t fsz;
	char* contents;
	struct json_file* jf;
	size_t nr;
	
	// check file size
	fseek(f, 0, SEEK_END);
	fsz = ftell(f);
	fseek(f, 0, SEEK_SET);
	
	contents = malloc(fsz+1);
	contents[fsz] = 0; // some crt functions might read past the end otherwise
	
	nr = fread(contents, 1, fsz, f);

	jf = json_parse_string(contents, fsz);
		
	free(contents);
	
	return jf;
}

struct json_file* json_parse_string(char* source, size_t len) {
	struct json_lexer* jl;
	struct json_parser* jp;
	struct json_file* jf;
	
	jp = parse_token_stream(source, len);
	if(!jp) {
		//printf("JSON: failed to parse token stream \n");
		return NULL;
	}
	
	jf = calloc(1, sizeof(*jf));
	if(!jf) return NULL;
	
	
	if(jp->stack_cnt == 1) {	
		jf->root = jp->stack[1];
		//printf("JSON: failed to parse token stream (2)\n");	
	}
	// else some sort of error. probably EOI
	
	jf->error = jp->error;
	if(jf->error) {
		jf->error_line_num = jp->line_num;
		jf->error_char_num = jp->char_num;
		jf->error_str = json_get_err_str(jf->error);
	}
	
	free(jp);
	
	//json_dump_value(*jp->stack, 0, 10);
	//json_dump_value(jf->root, 0, 10);
	return jf;
}


static void free_array(struct json_value* arr) {
	
	struct json_link* n, *p;
	
	n = arr->arr.head;
	while(n) {
		
		json_free(n->v);
		
		p = n;
		n = n->next;
		
		free(p);
	}
	
	free(arr);
}


static void free_obj(struct json_value* o) {
	size_t freed = 0;
	size_t i;
	
	for(i = 0; i < o->obj.alloc_size && freed < o->len; i++) {
		struct json_obj_field* b;
		
		b = &o->obj.buckets[i];
		if(b->key == NULL) continue;
		
		json_free(b->value);
		freed++;
	}
	
	free(o->obj.buckets);
	free(o);
}


// must manage v's memory manually
void json_free(struct json_value* v) {
	if(!v) return;
	
	switch(v->type) {
		case JSON_TYPE_STRING:
		case JSON_TYPE_COMMENT_SINGLE:
		case JSON_TYPE_COMMENT_MULTI:
			free(v->s);
			break;
		
		case JSON_TYPE_OBJ:
			free_obj(v);
			break;
			
		case JSON_TYPE_ARRAY:
			free_array(v);
			break;
	}
}


void json_file_free(struct json_file* jsf){
	json_free(jsf->root);
	if(jsf->lex_info) free(jsf->lex_info);
	free(jsf);
}


#define tc(x, y, z) case x: dbg_printf(#x ": " y "\n", z); break;
#define tcl(x) case x: dbg_printf(#x "\n"); break;

static void dbg_print_token(struct token* ts) {
	switch(ts->tokenType) {
		tcl(TOKEN_NONE)
		tcl(TOKEN_ARRAY_START)
		tcl(TOKEN_ARRAY_END)
		tcl(TOKEN_OBJ_START)
		tcl(TOKEN_OBJ_END)
		tc(TOKEN_STRING, "%s", ts->val->s)
		tc(TOKEN_NUMBER, "%d", (int)ts->val->n)
		tcl(TOKEN_NULL)
		tcl(TOKEN_INFINITY)
		tcl(TOKEN_UNDEFINED)
		tcl(TOKEN_NAN)
		tc(TOKEN_LABEL, "%s", ts->val->s)
		tcl(TOKEN_COMMA)
		tcl(TOKEN_COLON)
		tc(TOKEN_COMMENT, "%s", ts->val->s)
	}
}


static void dbg_print_value(struct json_value* v) {
	if(v == ROOT_VALUE) {
		dbg_printf("ROOT_VALUE sentinel\n");
		return;
	}
	if(v == RESUME_ARRAY) {
		dbg_printf("RESUME_ARRAY sentinel\n");
		return;
	}
	if(v == RESUME_OBJ) {
		dbg_printf("RESUME_OBJ sentinel\n");
		return;
	}
	
	switch(v->type) {
		case JSON_TYPE_UNDEFINED: dbg_printf("undefined\n"); break;
		case JSON_TYPE_NULL: dbg_printf("null\n"); break;
		case JSON_TYPE_INT: dbg_printf("int: %d\n", (int)v->n); break;
		case JSON_TYPE_DOUBLE: dbg_printf("double %f\n", v->d); break;
		case JSON_TYPE_STRING: dbg_printf("string: \"%s\"\n", v->s); break;
		case JSON_TYPE_OBJ: dbg_printf("object [%d]\n", (int)v->len); break;
		case JSON_TYPE_ARRAY: dbg_printf("array [%d]\n", (int)v->len); break;
		case JSON_TYPE_COMMENT_SINGLE: dbg_printf("comment, single\n"); break;
		case JSON_TYPE_COMMENT_MULTI: dbg_printf("comment, multiline\n"); break;
		default: 
			dbg_printf("unknown enum: %d\n", v->type);
	}
}

static void dbg_dump_stack(struct json_parser* jp, int depth) {
	int i = 0;
	return;
	for(i = 0; i < depth && i < jp->stack_cnt; i++) {
		dbg_parser_indent();
		dbg_printf("%d: ", i);
		dbg_print_value(jp->stack[jp->stack_cnt - i - 1]);
	}
}

char* json_get_type_str(enum json_type t) {
	switch(t) {
		case JSON_TYPE_UNDEFINED: return "undefined";
		case JSON_TYPE_NULL: return "null";
		case JSON_TYPE_INT: return "integer";
		case JSON_TYPE_DOUBLE: return "double";
		case JSON_TYPE_STRING: return "string";
		case JSON_TYPE_OBJ: return "object";
		case JSON_TYPE_ARRAY: return "array";
		case JSON_TYPE_COMMENT_SINGLE: return "single-line comment";
		case JSON_TYPE_COMMENT_MULTI: return "multi-line comment";
		default: return "Invalid JSON type";
	}
}

char* json_get_err_str(enum json_error e) {
	switch(e) {
		case JSON_ERROR_NONE: return "No error";
		case JSON_ERROR_OOM: return "Out of memory";
		
		case JSON_LEX_ERROR_NULL_IN_STRING: return "Null byte found in string";
		case JSON_LEX_ERROR_NULL_BYTE: return "Null byte found in file";
		case JSON_LEX_ERROR_INVALID_STRING: return "Invalid string";
		case JSON_LEX_ERROR_UNEXPECTED_END_OF_INPUT: return "Unexpected end of input in lexer";
		case JSON_LEX_ERROR_INVALID_CHAR: return "Invalid character code";
		
		case JSON_PARSER_ERROR_CORRUPT_STACK: return "Parser stack corrupted";
		case JSON_PARSER_ERROR_STACK_EXHAUSTED: return "Parser stack prematurely exhausted";
		case JSON_PARSER_ERROR_UNEXPECTED_EOI: return "Unexpected end of input";
		case JSON_PARSER_ERROR_UNEXPECTED_TOKEN: return "Unexpected token";
		case JSON_PARSER_ERROR_BRACE_MISMATCH: return "Brace mismatch";
		case JSON_PARSER_ERROR_BRACKET_MISMATCH: return "Bracket mismatch";
		default: return "Invalid Error Code";
	}
}

struct json_value* json_deep_copy(struct json_value* v) {
	struct json_value* c;

	c = malloc(sizeof(*c));
	c->type = v->type;

	switch(v->type) {
		default:
		case JSON_TYPE_INT:
		case JSON_TYPE_DOUBLE:
			c->n = v->n;
			c->base = v->base;
			break;

		case JSON_TYPE_ARRAY:
			c->len = v->len;

			if(v->len == 0) {
				c->arr.head = NULL;
				c->arr.tail = NULL;
			}
			else {
				struct json_link* cl = NULL;
				struct json_link* cl_last, *vl;
				
				cl_last = NULL;
				vl = v->arr.head;
				
				while(vl) {
					cl = malloc(sizeof(*cl));

					cl->prev = cl_last;
					if(cl_last) {
						cl_last->next = cl;
					}
					else {
						c->arr.head = cl;
					}

					cl->v = json_deep_copy(vl->v);
					
					cl_last = cl;
					vl = vl->next;
				}
				
				cl->next = NULL;
				c->arr.tail = cl;
			}

			break;

		case JSON_TYPE_OBJ:
			c->obj.alloc_size = v->obj.alloc_size;
			c->len = v->len;

			c->obj.buckets = calloc(1, sizeof(c->obj.buckets) * c->obj.alloc_size);

			for(size_t i = 0, j = 0; j < v->len && i < v->obj.alloc_size; i++) {
				if(v->obj.buckets[i].key) { 
					c->obj.buckets[i].key = strdup(v->obj.buckets[i].key);	
					c->obj.buckets[i].hash = v->obj.buckets[i].hash;
					c->obj.buckets[i].value = json_deep_copy(v->obj.buckets[i].value);
					j++;
				}
			}

			break;
	}
	
	return c;
}


// scalar and disparate types take the value of from
// appends arrays
// recursively merges objects
void json_merge(struct json_value* into, struct json_value* from) {
	
	// append two arrays
	if(into->type == JSON_TYPE_ARRAY && from->type == JSON_TYPE_ARRAY) {
		struct json_link* fl;

		fl = from->arr.head;
		while(fl) {
			json_array_push_tail(into, json_deep_copy(fl->v));
		}

		return;
	}
	
	// disparate or scalar types
	if(into->type != JSON_TYPE_OBJ || from->type != JSON_TYPE_OBJ) {
		
		// clean out into first
		if(into->type == JSON_TYPE_ARRAY) {
			free_array(into);
		}
		else if(into->type == JSON_TYPE_OBJ) {
			free_obj(into);
		}

		// deep-copy an array or obect from value
		if(from->type == JSON_TYPE_ARRAY || from->type == JSON_TYPE_OBJ) {
			struct json_value* tmp;
			tmp = json_deep_copy(from);
			memcpy(into, tmp, sizeof(*into));
			free(tmp);

			return;
		}
		
		// simple copy of scalars
		memcpy(into, from, sizeof(*into));

		return;
	}

	// merge objects
	void* fi = NULL;
	char* key;
	struct json_value* fv;

	while(json_obj_next(from, &fi, &key, &fv)) {
		struct json_value* iv;

		if(json_obj_get_key(into, key, &iv)) {
			json_obj_set_key(into, key, json_deep_copy(fv));
		}
		else { // key exists in into
			json_merge(iv, fv);
		}
	}
	
}



static void spaces(int depth, int w) {
	int i;
	for(i = 0; i < depth * w; i++) putchar(' ');
}

// shitty recursive fn for debugging
void json_dump_value(struct json_value* root, int cur_depth, int max_depth) {
	
	void* iter;
	char* key;
	struct json_value* v;
	
	//printf("-----------------------------------\n\n\n");
	
	if(root->type == JSON_TYPE_ARRAY) {
		spaces(cur_depth, 4);
		dbg_printf("[\n");
		
		// do shit
		
		spaces(cur_depth, 4);
		dbg_printf("]\n");
	}
	else if(root->type == JSON_TYPE_OBJ) {
		dbg_printf("{\n");
		
		// do shit
		iter = NULL;
		while(json_obj_next(root, &iter, &key, &v)) {
			//printf("looping\n;");
			spaces(cur_depth, 4);
			dbg_printf("%s: ", key);
			json_dump_value(v, cur_depth+1, max_depth);
		}
		
		spaces(cur_depth, 4);
		dbg_printf("}\n");
	}
	else {
		//printf("here");
		dbg_print_value(root);
		dbg_printf("\n");
	}
	
	
}




// JSON output

static void sb_cat_escaped(struct json_write_context* ctx, char* str);
static void json_obj_to_string(struct json_write_context* sb, struct json_value* obj);
static void json_arr_to_string(struct json_write_context* sb, struct json_value* arr);

struct json_string_buffer* json_string_buffer_create(size_t initSize) {
	struct json_string_buffer* b;
	b = malloc(sizeof(*b));
	
	b->length = 0;
	b->alloc = initSize;
	b->buf = malloc(initSize * sizeof(*b->buf));
	b->buf[0] = 0;
	
	return b;
}

void json_string_buffer_free(struct json_string_buffer* sb) {
	free(sb->buf);
	sb->buf = NULL;
	sb->length = 0;
	sb->alloc = 0;
}

static void sb_check(struct json_string_buffer* sb, size_t more) {
	char* tmp;

	if(sb->length + 1 + more > sb->alloc) {
		tmp = realloc(sb->buf, sb->alloc * 2);
		if(tmp) {
			sb->buf = tmp;
			sb->alloc *= 2;
		}
		else {
			//fprintf(stderr, "c_json: Memory allocation failed\n");
		}
	} 
}

// checks size and concatenates string
// does not check for newlines re: line_len
static void sb_cat(struct json_string_buffer* sb, char* str) {
	// TODO: optimize
	size_t len = strlen(str);
	sb_check(sb, len);
	strcat(sb->buf, str);
	sb->length += len;
	sb->line_len += len; 
}

// checks size and concatenates a single char
static void sb_putc(struct json_string_buffer* sb, int c) {
	// TODO: optimize
	
	sb_check(sb, 1);
	sb->buf[sb->length] = c;
	sb->buf[sb->length + 1] = 0;
	sb->length++;
	sb->line_len = c == '\n' ? 0 : sb->line_len + 1; 
}

static char* sb_tail_check(struct json_string_buffer* sb, int more) {
	sb_check(sb, more);
	return sb->buf + sb->length;
}

#define sb_tail_catf(sb, fmt, ...) \
do { \
	size_t _len = snprintf(NULL, 0, fmt, ##__VA_ARGS__); \
	sprintf(sb_tail_check(sb, _len), fmt, ##__VA_ARGS__); \
	sb->length += _len; \
	sb->line_len += _len; \
} while(0);

void json_stringify(struct json_write_context* ctx, struct json_value* v) {
	struct json_string_buffer* sb = ctx->sb;
	char* float_format = ctx->fmt.floatFormat ? ctx->fmt.floatFormat : "%f"; 
	char qc;
	
	if(!v) {
		//fprintf(stderr, "NULL value passed to %s()\n", __func__);
		return;
	}
	
	switch(v->type) {
		case JSON_TYPE_UNDEFINED:
			sb_cat(sb, "undefined");
			break;
			
		case JSON_TYPE_NULL: 
			sb_cat(sb, "null"); 
			break;
			
		case JSON_TYPE_INT: // 2
			sb_tail_catf(sb, "%ld", v->n); // TODO: handle bases, formats
			break;
			
		case JSON_TYPE_DOUBLE: 
			sb_tail_catf(sb, float_format, v->d); // TODO: handle infinity, nan, etc
			break;
			
		case JSON_TYPE_STRING:
			qc = ctx->fmt.useSingleQuotes ? '\'' : '"';
			sb_putc(sb, qc);
			sb_cat_escaped(ctx, v->s); 
			sb_putc(sb, qc);
			break;
			
		case JSON_TYPE_OBJ: 
			json_obj_to_string(ctx, v);
			break;
			
		case JSON_TYPE_ARRAY: // 6
			json_arr_to_string(ctx, v);
			break;
			
		case JSON_TYPE_COMMENT_SINGLE: // TODO: handle linebreaks in the comment string
			sb_tail_catf(sb, "//%s\n", v->s);
			break;
			
		case JSON_TYPE_COMMENT_MULTI: // TODO: clean "*/" out of the comment string
			sb_tail_catf(sb, "/* %s */\n", v->s);
			break;
			
		// default:
			// fprintf(stderr, "c_json: unknown type in json_value_to_string\n");
	}
}




static void ctx_indent(struct json_write_context* ctx) {
	int i = 0; 
	int len = ctx->fmt.indentAmt * ctx->depth;
	
	char* c = sb_tail_check(ctx->sb, len);
	
	for(i = 0; i < len; i++) {
		c[i] = ctx->fmt.indentChar;
	}
	
	c[len] = 0;
	ctx->sb->length += len;
	
	ctx->sb->line_len += len;
}

static int line_too_long(struct json_write_context* ctx) {
	if(ctx->fmt.maxLineLength < 0) return 0;
	if(ctx->sb->line_len <= ctx->fmt.maxLineLength) return 0;
	return 1;
}



static void sb_cat_escaped(struct json_write_context* ctx, char* str) {
	struct json_string_buffer* sb = ctx->sb;
	char qc = ctx->fmt.useSingleQuotes ? '\'' : '"';
	
	while(*str) {
		char c = *str;
		
		if(c == qc) sb_putc(sb, '\\');
		sb_putc(sb, c);
		
		str++;
	}
	
}


static void json_arr_to_string(struct json_write_context* ctx, struct json_value* arr) {
	struct json_link* n;
	struct json_string_buffer* sb = ctx->sb;
	
	int multiline = arr->len >= (size_t)ctx->fmt.minArraySzExpand;
	
	sb_putc(sb, '[');
	
	if(multiline) sb_putc(sb, '\n');
	
	ctx->depth++;
	
	n = arr->arr.head;
	while(n) {
		
		if(multiline) ctx_indent(ctx);
		
		json_stringify(ctx, n->v);
		
		n = n->next;
		
		if(n) {
			sb_putc(sb, ',');
			if(!multiline) sb_putc(sb, ' ');
		}
		else if(ctx->fmt.trailingComma && multiline) sb_putc(sb, ',');
		
		if(multiline) sb_putc(sb, '\n');
		if(line_too_long(ctx)) {
			sb_putc(sb, '\n');
			ctx_indent(ctx);
		}
	}
	
	ctx->depth--;
	
	if(multiline) ctx_indent(ctx);
	
	sb_putc(sb, ']');
}


static int key_must_have_quotes(char* key) {
	if(isdigit(key[0])) return 1;
	return  strlen(key) != strspn(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$");
}


static void json_obj_to_string(struct json_write_context* ctx, struct json_value* obj) {
	size_t i;
	struct json_obj_field* f;
	struct json_string_buffer* sb = ctx->sb;
	
	int multiline = obj->len >= (size_t)ctx->fmt.minObjSzExpand;
	int noquotes = ctx->fmt.noQuoteKeys;
	char quoteChar = ctx->fmt.useSingleQuotes ? '\'' : '"';
	
	sb_putc(sb, '{');
	
	if(multiline) sb_putc(sb, '\n');
	
	ctx->depth++;
	
	size_t n = obj->len;
	for(i = 0; i < obj->obj.alloc_size; i++) {
		f = &obj->obj.buckets[i];
		if(f->key == NULL) continue;
		
		if(multiline) ctx_indent(ctx);
		
		int needquotes = key_must_have_quotes(f->key); 
		
		if(!noquotes || needquotes) {
			sb_putc(sb, quoteChar);
			sb_cat_escaped(ctx, f->key);
			sb_putc(sb, quoteChar);
		}
		else sb_cat(sb, f->key);
		
		sb_putc(sb, ':');
		if(ctx->fmt.objColonSpace) sb_putc(sb, ' ');
		
		json_stringify(ctx, f->value);
		
		if(n-- > 1) {
			sb_putc(sb, ',');
			if(!multiline) sb_putc(sb, ' ');
		}
		else if(ctx->fmt.trailingComma && multiline) sb_putc(sb, ',');
		
		if(multiline) sb_putc(sb, '\n');
		if(line_too_long(ctx)) {
			sb_putc(sb, '\n');
			ctx_indent(ctx);
		}
	}
	
	ctx->depth--;
	
	if(multiline) ctx_indent(ctx);
	sb_putc(sb, '}');
}


struct json_value* json_new_str(char* s) {
	return json_new_strn(s, strlen(s));
}

struct json_value* json_new_strn(char* s, size_t len) {
	struct json_value* v;
	
	v = malloc(sizeof(*v));
	v->type = JSON_TYPE_STRING;
	v->s = strndup(s, len);
	v->len = len;
	v->base = 0;
	
	return v;
}

struct json_value* json_new_double(double d) {
	struct json_value* v;
	
	v = malloc(sizeof(*v));
	v->type = JSON_TYPE_DOUBLE;
	v->d = d;
	v->len = 0;
	v->base = 0;
	
	return v;
}

struct json_value* json_new_int(int64_t n) {
	struct json_value* v;
	
	v = malloc(sizeof(*v));
	v->type = JSON_TYPE_INT;
	v->n = n;
	v->len = 0;
	v->base = 0;
	
	return v;
}

struct json_value* json_new_array() {
	struct json_value* v;
	
	v = malloc(sizeof(*v));
	v->type = JSON_TYPE_ARRAY;
	v->arr.head = NULL;
	v->arr.tail = NULL;
	v->len = 0;
	v->base = 0;
	
	return v;
}

struct json_value* json_new_object(size_t initial_alloc_size) {
	
	struct json_value* obj = malloc(sizeof(*obj));
	
	obj->type = JSON_TYPE_OBJ;
	obj->len = 0;
	obj->base = 0;
	obj->obj.alloc_size = initial_alloc_size;
	obj->obj.buckets = calloc(1, sizeof(*obj->obj.buckets) * obj->obj.alloc_size);
	if(!obj->obj.buckets) {
		free(obj);
		return NULL;
	}
	
	return obj;
}

struct json_value* json_new_null() {
	struct json_value* v;
	
	v = malloc(sizeof(*v));
	v->type = JSON_TYPE_NULL;
	
	return v;
}

struct json_value* json_new_undefined() {
	struct json_value* v;
	
	v = malloc(sizeof(*v));
	v->type = JSON_TYPE_UNDEFINED;
	
	return v;
}

struct json_value* json_new_true() {
	struct json_value* v;
	
	v = malloc(sizeof(*v));
	v->type = JSON_TYPE_BOOL;
	v->n = 1; // true
	v->len = 0;
	v->base = 0;
	
	return v;
}

struct json_value* json_new_false() {
	struct json_value* v;
	
	v = malloc(sizeof(*v));
	v->type = JSON_TYPE_BOOL;
	v->n = 0; // false
	v->len = 0;
	v->base = 0;
	
	return v;
}



