
#define __STDC_WANT_LIB_EXT2__ 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>

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
	int line_num;
	int char_num;
	enum token_type tokenType;
	struct json_value* val;
};

struct json_lexer {
	char* source;
	char* end;
	size_t source_len;
	
	// output info
	struct token* token_stream;
	size_t ts_cnt;
	size_t ts_alloc;
	
	// stateful info
	char* head;
	int line_num; // these are 1-based
	int char_num;
	int error;
};



struct json_parser {
	struct token* token_stream;
	struct token* cur_token;
	struct token* last_token;
	int ts_len;
	
	struct json_value** stack;
	int stack_cnt;
	int stack_alloc;
	
	int error;
};


// sentinels for the parser stack

// the slot above contains an array, merge into it
struct json_value* RESUME_ARRAY = (struct json_value*)&RESUME_ARRAY;

// the slot above contains a label, the slot above that contains the object to merge into
struct json_value* RESUME_OBJ = (struct json_value*)&RESUME_OBJ;
struct json_value* ROOT_VALUE = (struct json_value*)&ROOT_VALUE;



static void dbg_print_token(struct token* ts);
static void dbg_dump_stack(struct json_parser* jp, int depth);


struct json_array* json_array_create() {

	struct json_array* arr;
	
	arr = malloc(sizeof(*arr));
	if(!arr) return NULL;
	
	arr->length = 0;
	arr->head = NULL;
	arr->tail = NULL;
	
	return arr;
}


// pushes the tail
int json_array_push_tail(struct json_value* arr, struct json_value* val) {

	struct json_array* a;
	struct json_array_node* node;
	
	a = arr->v.arr;
	
	node = malloc(sizeof(*node));
	if(!node) return 1;
	
	node->next = NULL;
	node->prev = a->tail;
	node->value = val;
	
	if(a->length == 0) {
		a->head = node;
	}
	else {
		a->tail->next = node;
	}
	
	a->tail = node;
	a->length++;
	
	return 0;
}

// pops the tail
int json_array_pop_tail(struct json_value* arr, struct json_value** val) {

	struct json_array* a;
	struct json_array_node* t;
	
	a = arr->v.arr;
	
	if(a->length == 0) {
		return 1;
	}
	
	a->length--;
	
	*val = a->tail->value;
	
	if(a->length > 0) {
		t = a->tail;
		a->tail = a->tail->prev;
		a->tail->next = NULL;
	}
	else {
		a->head = a->tail = NULL;
	}
	
	free(t);
	
	return 0;
}


size_t json_array_length(struct json_value* arr) {
	size_t len;
	struct json_array_node* n;
	
	if(arr->type != JSON_TYPE_ARRAY) {
		return 0;
	}
	
	len = 0;
	n = arr->v.arr->head;
	while(n) {
		len++;
		n = n->next;
	}
	
	return len;
}


struct json_obj* json_obj_create(size_t initial_alloc_size) {
	
	struct json_obj* obj;

	obj = malloc(sizeof(*obj));
	if(!obj) return NULL;
	
	obj->fill = 0;
	obj->alloc_size = initial_alloc_size;
	obj->buckets = calloc(1, sizeof(*obj->buckets) * obj->alloc_size);
	if(!obj->buckets) {
		free(obj);
		return NULL;
	}
	
	return obj;
}


// uses a truncated 128bit murmur3 hash
static uint64_t hash_key(char* key, size_t len) {
	uint64_t hash[2];
	
	// len is optional
	if(len == -1) len = strlen(key);
	
	MurmurHash3_x64_128(key, len, MURMUR_SEED, hash);
	
	return hash[0];
}

static int64_t find_bucket(struct json_obj* obj, uint64_t hash, char* key) {
	int64_t startBucket, bi;
	
	bi = startBucket = hash % obj->alloc_size; 
	
	do {
		struct json_obj_field* bucket;
		
		bucket = &obj->buckets[bi];
		
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
		
		bi = (bi + 1) % obj->alloc_size;
	} while(bi != startBucket);
	
	printf("CJSON: error in find_bucket\n");
	// should never reach here if the table is maintained properly
	return -1;
}


// should always be called with a power of two
static int json_obj_resize(struct json_obj* obj, int newSize) {
	struct json_obj_field* old, *op;
	size_t oldlen = obj->alloc_size;
	int64_t i, n, bi;
	
	old = op = obj->buckets;
	
	obj->alloc_size = newSize;
	obj->buckets = calloc(1, sizeof(*obj->buckets) * newSize);
	if(!obj->buckets) return 1;
	
	for(i = 0, n = 0; i < oldlen && n < (int64_t)obj->fill; i++) {
		if(op->key == NULL) {
			op++;
			continue;
		}
		
		bi = find_bucket(obj, op->hash, op->key);
		obj->buckets[bi].value = op->value;
		obj->buckets[bi].hash = op->hash;
		obj->buckets[bi].key = op->key;
		
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
	struct json_obj* o;
	
	o = obj->v.obj;
	
	hash = hash_key(key, -1);
	
	bi = find_bucket(o, hash, key);
	if(bi < 0 || o->buckets[bi].key == NULL) {
		*val = NULL;
		return 1;
	}
	
	*val = o->buckets[bi].value; 
	return 0;
}

// zero for success
int json_obj_set_key(struct json_value* obj, char* key, struct json_value* val) {
	uint64_t hash;
	int64_t bi;
	struct json_obj* o;
	
	o = obj->v.obj;
	
	// check size and grow if necessary
	if((float)o->fill / (float)o->alloc_size >= 0.75) {
		json_obj_resize(o, o->alloc_size * 2);
	}
	
	hash = hash_key(key, -1);
	
	bi = find_bucket(o, hash, key);
	if(bi < 0) return 1;
	
	o->buckets[bi].value = val;
	o->buckets[bi].key = key;
	o->buckets[bi].hash = hash;
	o->fill++;
	
	return 0;
}


// will probably be changed or removed later
// coerces and strdup's the result
// returns null if the key does not exist
char* json_obj_key_as_string(struct json_value* obj, char* key) {
	json_value_t* val;
	char* str;
	
	if(json_obj_get_key(obj, key, &val)) {
		return NULL;
	}
	
	json_as_string(val, &str);
	return strdup(str);
}


// returns pointer to the internal string, or null if it's not a string
char* json_obj_get_string(struct json_value* obj, char* key) {
	json_value_t* val;
	
	if(json_obj_get_key(obj, key, &val)) {
		return NULL;
	}
	
	if(val->type != JSON_TYPE_STRING) {
		return NULL;
	}
	
	return val->v.str;
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
	
	return val->v.integer;
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
	
	return val->v.dbl;
}


// returns the json_value strut for a key, or null if it doesn't exist
struct json_value* json_obj_get_val(struct json_value* obj, char* key) {
	json_value_t* val;
	
	if(json_obj_get_key(obj, key, &val)) {
		return NULL;
	}

	return val;
}


// number of keys in an object
// -1 on error
int json_obj_length(struct json_value* val) {
	if(val->type != JSON_TYPE_OBJ) return -1;
	
	return val->v.obj->fill;
}


// iteration. no order. results undefined if modified while iterating
// returns 0 when there is none left
// set iter to NULL to start
int json_obj_next(struct json_value* val, void** iter, char** key, struct json_value** value) { 
	struct json_obj_field* b = *iter;
	struct json_obj* obj;
	
	if(val->type != JSON_TYPE_OBJ) return 1;
	obj = val->v.obj;
	
	if(obj->fill == 0) {
		return 0;
	}
	
	// a tiny bit of idiot-proofing
	if(b == NULL) b = &obj->buckets[-1];
	//printf("alloc size: %d\n", obj->alloc_size);
	do {
		b++;
		if(b >= obj->buckets + obj->alloc_size) {
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
*/
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
	
	l = o->fill;
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


/*
type coercion rules:
undefined/null -> int = 0
numbers, as you would expect, according to C type conversion rules
obj/array -> number/string = error
string/comment -> string = string
number -> string = sprintf, accoding to some nice rules.
string -> number = strtod/i

JSON_TYPE_INT is assumed to be C int

numbers over 2^63 are not properly supported yet. they will be truncated to 0
*/

// returns 0 if successful
int json_as_type(struct json_value* v, enum json_type t, void* out) { 
	int ret;
	int64_t i;
	double d;
	
	if(!v) return 1;
	
	switch(t) { // actual type

		case JSON_TYPE_INT:
			ret = json_as_int(v, &i);
			if(!ret) *((int*)out) = i;
			return 0;
			
		case JSON_TYPE_DOUBLE: return json_as_double(v, out);
		case JSON_TYPE_STRING: return json_as_string(v, out);
		
		case JSON_TYPE_OBJ: *((struct json_obj**)out) = v->v.obj; return 0;
		case JSON_TYPE_ARRAY: *((struct json_array**)out) = v->v.arr; return 0;

			
		case JSON_TYPE_FLOAT:
			ret = json_as_double(v, &d);
			if(!ret) *((float*)out) = d;
			return ret;
			
		case JSON_TYPE_INT8:
			ret = json_as_int(v, &i);
			if(!ret) *((int8_t*)out) = i;
			return ret;
			
		case JSON_TYPE_INT16:
			ret = json_as_int(v, &i);
			if(!ret) *((int16_t*)out) = i;
			return ret;
			
		case JSON_TYPE_INT32: 
			ret = json_as_int(v, &i);
			if(!ret) *((int32_t*)out) = i;
			return ret;
			
		case JSON_TYPE_INT64: return json_as_double(v, out);
		
		case JSON_TYPE_UINT8:
			ret = json_as_int(v, &i);
			if(!ret) *((uint8_t*)out) = i;
			return ret;
			
		case JSON_TYPE_UINT16:
			ret = json_as_int(v, &i);
			if(!ret) *((uint16_t*)out) = i;
			return ret;
			
		case JSON_TYPE_UINT32: 
			ret = json_as_int(v, &i);
			if(!ret) *((uint32_t*)out) = i;
			return ret;
			
		case JSON_TYPE_UINT64: 
			ret = json_as_int(v, &i);
			if(!ret) *((uint64_t*)out) = i;
			return ret;
			
		case JSON_TYPE_UNDEFINED:
		case JSON_TYPE_NULL:
		case JSON_TYPE_COMMENT_SINGLE:
		case JSON_TYPE_COMMENT_MULTI:
		default:
			return 1;
	}
	
	
	
}

// returns 0 for success
int json_as_int(struct json_value* v, int64_t* out) {
	switch(v->type) { // actual type
		case JSON_TYPE_UNDEFINED:
		case JSON_TYPE_NULL:
			*out = 0;
			return 0;
			
		case JSON_TYPE_INT:
			*out = v->v.integer;
			return 0;
			
		case JSON_TYPE_DOUBLE:
			*out = v->v.dbl;
			return 0;
			
		case JSON_TYPE_STRING:
			*out = strtol(v->v.str, NULL, 0);
			return 0;
			
		case JSON_TYPE_OBJ:
		case JSON_TYPE_ARRAY:
		case JSON_TYPE_COMMENT_SINGLE:
		case JSON_TYPE_COMMENT_MULTI:
		default:
			*out = 0;
			return 1;
	}
}

// returns 0 for success
int json_as_double(struct json_value* v, double* out) {
	switch(v->type) { // actual type
		case JSON_TYPE_UNDEFINED:
		case JSON_TYPE_NULL:
			*out = 0.0;
			return 0;
			
		case JSON_TYPE_INT:
			*out = v->v.integer;
			return 0;
			
		case JSON_TYPE_DOUBLE:
			*out = v->v.dbl;
			return 0;
			
		case JSON_TYPE_STRING:
			*out = strtod(v->v.str, NULL);
			return 0;
			
		case JSON_TYPE_OBJ:
		case JSON_TYPE_ARRAY:
		case JSON_TYPE_COMMENT_SINGLE:
		case JSON_TYPE_COMMENT_MULTI:
		default:
			*out = 0.0;
			return 1;
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
int json_as_string(struct json_value* v, char** out) {
	char* buf;
	size_t len;
	
	switch(v->type) { // actual type
		case JSON_TYPE_UNDEFINED:
			*out = "undefined";
			return 0;
			
		case JSON_TYPE_NULL:
			*out = "null";
			return 0;
			
		case JSON_TYPE_INT:
			*out = a_sprintf("%ld", v->v.integer); // BUG might leak memory
			return 0;
			
		case JSON_TYPE_DOUBLE:
			*out = a_sprintf("%f", v->v.dbl);
			return 0;
			
		case JSON_TYPE_COMMENT_SINGLE:
		case JSON_TYPE_COMMENT_MULTI:
		case JSON_TYPE_STRING:
			*out = v->v.str;
			return 0;
			
		case JSON_TYPE_OBJ:
			*out = "[Object]";
			return 0;
			
		case JSON_TYPE_ARRAY:
			*out = "[Array]";
			return 0;
		
		default:
			*out = "";
			return 1;
	}
}


int json_as_float(struct json_value* v, float* f) {
	return json_as_type(v, JSON_TYPE_FLOAT, f); 
}




#define check_oom(x) \
if(x == NULL) { \
	jl->error = JSON_ERROR_OOM; \
	return 1; \
}

// out must be big enough, at least as big as in+1 just to be safe
// appends a null to out, but is also null-safe
static int decode_c_escape_str(char* in, char* out, size_t len) {
	int i;
	
	for(i = 0; i < len; i++) {
		if(*in == '\\') {
			in++;
			i++;
			switch(*in) {
				case '\'': *out = '\''; break; 
				case '"': *out = '"'; break; 
				case '`': *out = '`'; break; 
				case '?': *out = '?'; break; 
				case '0': *out = '\0'; break; 
				case 'r': *out = '\r'; break; 
				case 'n': *out = '\n'; break;
				case 'f': *out = '\f'; break;
				case 'a': *out = '\a'; break;
				case 'b': *out = '\b'; break;
				case 'v': *out = '\v'; break;
				case 't': *out = '\t'; break;
				case 'x': 
					// TODO: parse hex code
					*out = '?';
					break;
				case 'U':
					// TODO parse longer unicode
				case 'u': 
					// TODO: parse unicode
					*out = '?';
					break;
				// TODO: parse octal
					
				default:
					return 1;
			}
		}
		else {
			*out = *in;
		}
		
		out++;
		in++;
	}
	
	*out = '\0';
	
	return 0;
}

// move forward one char
static void lex_next_char(struct json_lexer* jl) {
	if(jl->error) { 
		printf("next char has error\n");
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
static int lex_push_token_val(struct json_lexer* jl, enum token_type t, struct json_value* val) {
	size_t cnt, alloc;
	void* tmp;
	
	cnt = jl->ts_cnt;
	
	// check size
	if(cnt >= jl->ts_alloc) {
		if(jl->ts_alloc == 0) jl->ts_alloc = 64;
		jl->ts_alloc *= 2;
		tmp = realloc(jl->token_stream, jl->ts_alloc * sizeof(*(jl->token_stream)));
		check_oom(tmp)
		
		jl->token_stream = tmp;
	}
	
	// copy data
	jl->token_stream[cnt].line_num = jl->line_num;
	jl->token_stream[cnt].char_num = jl->char_num;
	jl->token_stream[cnt].tokenType = t;
	jl->token_stream[cnt].val = val;
	
	jl->ts_cnt++;
	
	return 0;
}

// returns error code. val set to null
static int lex_push_token(struct json_lexer* jl, enum token_type t) {
	return lex_push_token_val(jl, t, NULL);
}


static int lex_string_token(struct json_lexer* jl) {
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
	//printf("error: %d\n", jl->error);
	
	len = se - jl->head - 1;
	
	str = malloc(len+1);
	check_oom(str)
	
	//printf("error: %d\n", jl->error);
	
	if(decode_c_escape_str(jl->head + 1, str, len)) {
		jl->error = JSON_LEX_ERROR_INVALID_STRING;
		return 1;
	}
//printf("error: %d\n", jl->error);
	// json value
	val = calloc(1, sizeof(*val));
	check_oom(val)
//	printf("error: %d\n", jl->error);
	val->type = JSON_TYPE_STRING;
	val->v.str = str;
	
	lex_push_token_val(jl, TOKEN_STRING, val);
	
	// advance to the end of the string
	jl->head = se;
	jl->char_num = char_num;
	jl->line_num += lines;
	
//	printf("head %c\n", *jl->head);
//	printf("lc/line/char [%d/%d/%d]\n", jl->head - jl->source, jl->line_num, jl->char_num);
	
	return 0;
}


static int lex_number_token(struct json_lexer* jl) {
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
		val->v.dbl = strtod(s, &e);
		val->type = JSON_TYPE_DOUBLE;
		if(negate) val->v.dbl *= -1;
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
		
		val->v.integer = strtol(s, &e, base);
		val->type = JSON_TYPE_INT;
		if(negate) val->v.integer *= -1;
	}
	
	val->info.base = base;
	
	lex_push_token_val(jl, TOKEN_NUMBER, val);
	
	// advance to the end of the string
	jl->char_num += e - jl->head - 1;
	jl->head = e - 1;
	
//	printf("head %c\n", *jl->head);
//	printf("lc/line/char [%d/%d/%d]\n", jl->head - jl->source, jl->line_num, jl->char_num);
	
	return 0;
}


static int lex_label_token(struct json_lexer* jl) {
	size_t len;
	struct json_value* val;
	char* str;
	char* se = jl->head;
	
	int char_num = jl->char_num;

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
	//printf("error: %d\n", jl->error);
	
	len = se - jl->head;
	// TODO: check for null, infinity, undefined, nan
	
	str = malloc(len+1);
	check_oom(str)
	
	//printf("error: %d\n", jl->error);
	
	strncpy(str, jl->head, len);
	str[len] = 0;// BUG sometiems this gets messed up?
	
// printf("error: %d\n", jl->error);
	// json value
	val = calloc(1, sizeof(*val));
	check_oom(val)
	//printf("error: %d\n", jl->error);
	val->type = JSON_TYPE_STRING;
	val->v.str = str;
	
	lex_push_token_val(jl, TOKEN_LABEL, val);
	
	// advance to the end of the string
	jl->head = se - 1;
	jl->char_num = char_num - 1;
	
	//printf("head %c\n", *jl->head);
	//printf("lc/line/char [%d/%d/%d]\n", jl->head - jl->source, jl->line_num, jl->char_num);
	
	return 0;
}



static int lex_comment_token(struct json_lexer* jl) {
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
static int lex_nibble(struct json_lexer* jl) {
	
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
			
			// lex error
			jl->error = JSON_LEX_ERROR_INVALID_CHAR;
			return 1;
	}
	
	//printf("lol\n");
	
	lex_next_char(jl);
	
	return jl->error || jl->head >= jl->end;
}

// this is the lexer
static struct json_lexer* tokenize_string(char* source, size_t len) {
	
	struct json_lexer* jl;
	
	char open;
	char* data;
	char* start;
	
	
	// set up the lexer struct
	jl = calloc(1, sizeof(*jl));
	if(!jl) return NULL;
	
	jl->source = source;
	jl->end = source + len;
	jl->source_len = len;
	
	jl->head = source;
	jl->line_num = 1; // these are 1-based
	jl->char_num = 1;
	
	// all the work done here
	while(!lex_nibble(jl));// printf("*");
	
	if(jl->error) {
		printf("error code: %d\n", jl->error);
	}
	
	return jl;
}

static int parser_indent_level = 0;
static void dbg_parser_indent() { return;
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
	struct json_array* arr;
	
	val = malloc(sizeof(*val));
	if(!val) {
		jp->error = JSON_ERROR_OOM;
		return;
	}
	
	arr = json_array_create();
	if(!arr) {
		jp->error = JSON_ERROR_OOM;
		return;
	}
	
	val->type = JSON_TYPE_ARRAY;
	val->v.arr = arr;
	
	parser_push(jp, val);
}

static void parser_push_new_object(struct json_parser* jp) {
	
	struct json_value* val;
	struct json_obj* obj;
	
	val = malloc(sizeof(*val));
	if(!val) {
		jp->error = JSON_ERROR_OOM;
		return;
	}
	
	obj = json_obj_create(4);
	
	val->type = JSON_TYPE_OBJ;
	val->v.obj = obj;
	
// 	printf("vt: %d\n", val);
	dbg_dump_stack(jp, 3);
	parser_push(jp, val);
	dbg_dump_stack(jp, 4);
}

// parser helper
static inline struct token* consume_token(struct json_parser* jp) {
	if(jp->cur_token > jp->last_token) {
		jp->error = JSON_PARSER_ERROR_UNEXPECTED_EOI;
		return NULL;
	}
	
	dbg_parser_indent();
	dbg_printf("ct-%d-", jp->cur_token[1].line_num); dbg_print_token(jp->cur_token + 1);
	
	return ++jp->cur_token;
}


// not used atm. comments will probably be moved to a side channel
static void consume_comments(struct json_parser* jp) {
	while(1) {
		if(jp->cur_token->tokenType != TOKEN_COMMENT) break;
		
		parser_push(jp, jp->cur_token->val);
		consume_token(jp);
	}
}

static void consume_commas(struct json_parser* jp) {
	while(1) {
		if(jp->cur_token[1].tokenType != TOKEN_COMMA) break;
		
		//parser_push(jp, jp->cur_token->val);
		consume_token(jp);
	}
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
	if(obj->type != JSON_TYPE_OBJ) { printf("invalid obj\n");
		
		/*
		dbg_printf("0 type: %d \n", v->type);
		dbg_printf("1 type: %d \n", l->type);
		dbg_printf("2 type: %d \n", obj->type);
		dbg_printf("3 type: %d \n", st[-3]->type);
		*/
		jp->error = JSON_PARSER_ERROR_CORRUPT_STACK;
		return;
	}
	
	// insert l:v into obj
	json_obj_set_key(obj, l->v.str, v);
	// BUG? free label value?
	
	jp->stack_cnt -= 2;
}


static struct json_parser* parse_token_stream(struct json_lexer* jl) {
	
	int i;
	struct json_parser* jp;
	struct token* tok;
	
	jp = calloc(1, sizeof(*jp));
	if(!jp) {
		return NULL;
	}
	
	tok = jl->token_stream;
	jp->token_stream = jl->token_stream;
	jp->cur_token = jl->token_stream;
	jp->last_token = jl->token_stream + jl->ts_cnt;

	i = 0;

#define next() if(!(tok = consume_token(jp))) goto UNEXPECTED_EOI;

	// the root value sentinel helps a few algorithms and marks a proper end of input
	parser_push(jp, ROOT_VALUE);

	PARSE_ARRAY:
		dbg_parser_indent();
		dbg_printf("\nparse_array\n");
		
		if(jp->cur_token >= jp->last_token) goto CHECK_END;
	
		// cycle: val, comma
		switch(tok->tokenType) {
		
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
						
						consume_commas(jp);
						next();
						goto PARSE_ARRAY;
					}
					else if(sentinel == RESUME_OBJ) {
						reduce_object(jp);
						
						consume_commas(jp);
						next();
						goto PARSE_OBJ;
					}
					else {
						goto INVALID_SENTINEL;
					}
				}
				consume_commas(jp);
				next();
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
				parser_push(jp, tok->val);
				reduce_array(jp);
				
				consume_commas(jp);
				next();
				
				goto PARSE_ARRAY;
		
			case TOKEN_OBJ_END: dbg_parser_indent();dbg_printf("BRACKET_MISMATCH\n");
				goto BRACKET_MISMATCH;
			
			case TOKEN_NONE:
			case TOKEN_LABEL:
			case TOKEN_COMMA:
			case TOKEN_COLON: 
			default: dbg_printf("UNEXPECTED_TOKEN\n");
				// invalid
				goto UNEXPECTED_TOKEN;
		}
	
		return NULL;
	
	PARSE_OBJ:
		dbg_parser_indent();dbg_printf("\nparse_obj\n");
	// cycle: label, colon, val, comma
	
		if(tok->tokenType == TOKEN_OBJ_END) {
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
					
					consume_commas(jp);
					next();
					goto PARSE_ARRAY;
				}
				else if(sentinel == RESUME_OBJ) {
					reduce_object(jp);
					
					consume_commas(jp);
					next();
					goto PARSE_OBJ;
				}
				else {
					goto INVALID_SENTINEL;
				}
			}
			
			dbg_printf("ending obj in begining of obj\n");
			goto UNEXPECTED_TOKEN;
		}
		else if(tok->tokenType != TOKEN_LABEL && tok->tokenType != TOKEN_STRING) {
			// error
			dbg_printf("!!!missing label\n");
			goto UNEXPECTED_TOKEN;
		}
		parser_push(jp, tok->val);
		dbg_dump_stack(jp, 5);
		next();
		
		if(tok->tokenType != TOKEN_COLON) {
			// error
			dbg_printf("!!!missing colon\n");
			goto UNEXPECTED_TOKEN;
		}
		next();

	
		switch(tok->tokenType) {
		
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
				parser_push(jp, tok->val);
				reduce_object(jp);
				
				consume_commas(jp);
				next();
				
				goto PARSE_OBJ;
		
			case TOKEN_ARRAY_END: dbg_parser_indent();dbg_printf("obj- BRACE_MISMATCH\n");
				goto BRACE_MISMATCH;
			
			case TOKEN_NONE:
			case TOKEN_LABEL:
			case TOKEN_COMMA:
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
	if(jp->error) printf("parsing error: %d\n", jp->error);
	return jp;
UNEXPECTED_EOI: // end of input
	dbg_printf("!!! UNEXPECTED_EOI\n");
	return NULL;
UNEXPECTED_TOKEN: dbg_printf("!!! UNEXPECTED_TOKEN\n");
	return NULL;
BRACE_MISMATCH: dbg_printf("!!! BRACE_MISMATCH\n");
	return NULL;
BRACKET_MISMATCH: dbg_printf("!!! BRACKET_MISMATCH\n");
	return NULL;
INVALID_SENTINEL: dbg_printf("!!! INVALID_SENTINEL\n");
	return NULL;
}


struct json_file* json_load_path(char* path) {
	struct json_file* jf;
	FILE* f;
	
	f = fopen(path, "rb");
	if(!f) {
		fprintf(stderr, "JSON: no such file: \"%s\"\n", path);
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
	
	jf = calloc(1, sizeof(*jf));
	if(!jf) return NULL;
	
	jl = tokenize_string(source, len);
	
	if(!jl) return NULL;
	
	jp = parse_token_stream(jl);
	if(!jp) {
		printf("JSON: failed to parse token stream\n");
		return NULL;
	}

	jf->root = jp->stack[1];
	//json_dump_value(*jp->stack, 0, 10);
	//json_dump_value(jf->root, 0, 10);
	return jf;
}


static void free_array(struct json_array* arr) {
	
	struct json_array_node* n, *p;
	
	n = arr->head;
	while(n) {
		
		json_free(n->value);
		
		p = n;
		n = n->next;
		
		free(p);
	}
	
	free(arr);
}


static void free_obj(struct json_obj* o) {
	size_t freed = 0;
	size_t i;
	
	for(i = 0; i < o->alloc_size && freed < o->fill; i++) {
		struct json_obj_field* b;
		
		b = &o->buckets[i];
		if(b->key == NULL) continue;
		
		json_free(b->value);
		freed++;
	}
	
	free(o->buckets);
	free(o);
}


// must manage v's memory manually
void json_free(struct json_value* v) {
	if(!v) return;
	
	switch(v->type) {
		case JSON_TYPE_STRING:
			free(v->v.str);
			break;
		
		case JSON_TYPE_OBJ:
			free_obj(v->v.obj);
			break;
			
		case JSON_TYPE_ARRAY:
			free_array(v->v.arr);
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
		tc(TOKEN_STRING, "%s", ts->val->v.str)
		tc(TOKEN_NUMBER, "%d", (int)ts->val->v.integer)
		tcl(TOKEN_NULL)
		tcl(TOKEN_INFINITY)
		tcl(TOKEN_UNDEFINED)
		tcl(TOKEN_NAN)
		tc(TOKEN_LABEL, "%s", ts->val->v.str)
		tcl(TOKEN_COMMA)
		tcl(TOKEN_COLON)
		tc(TOKEN_COMMENT, "%s", ts->val->v.str)
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
		case JSON_TYPE_INT: dbg_printf("int: %d\n", (int)v->v.integer); break;
		case JSON_TYPE_DOUBLE: dbg_printf("double %f\n", v->v.dbl); break;
		case JSON_TYPE_STRING: dbg_printf("string: \"%s\"\n", v->v.str); break;
		case JSON_TYPE_OBJ: dbg_printf("object [%d]\n", (int)v->v.obj->fill); break;
		case JSON_TYPE_ARRAY: dbg_printf("array [%d]\n", (int)v->v.arr->length); break;
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
		case JSON_LEX_ERROR_UNEXPECTED_END_OF_INPUT: return "Unexpected end of input (lexer)";
		case JSON_LEX_ERROR_INVALID_CHAR: return "Invalid character code";
		
		case JSON_PARSER_ERROR_CORRUPT_STACK: return "Parser stack corrupted";
		case JSON_PARSER_ERROR_STACK_EXHAUSTED: return "Parser stack prematurely exhausted";
		case JSON_PARSER_ERROR_UNEXPECTED_EOI: return "Unexpected end of input (parser)";
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
			c->v.integer = v->v.integer;
			c->info.base = v->info.base;
			break;

		case JSON_TYPE_ARRAY:
			c->v.arr = malloc(sizeof(*c->v.arr));
			v->v.arr->length = v->v.arr->length;

			if(v->v.arr->length == 0) {
				c->v.arr->head = NULL;
				c->v.arr->tail = NULL;
			}
			else {
				struct json_array_node* cl, *cl_last, *vl;
				
				cl_last = NULL;
				vl = v->v.arr->head;
				
				while(vl) {
					cl = malloc(sizeof(*cl));

					cl->prev = cl_last;
					if(cl_last) {
						cl_last->next = cl;
					}
					else {
						c->v.arr->head = cl;
					}

					cl->value = json_deep_copy(vl->value);
					
					cl_last = cl;
					vl = vl->next;
				}
				
				cl->next = NULL;
				c->v.arr->tail = cl;
			}

			break;

		case JSON_TYPE_OBJ:
			c->v.obj = malloc(sizeof(*c->v.obj));
			c->v.obj->alloc_size = v->v.obj->alloc_size;
			c->v.obj->fill = v->v.obj->fill;

			c->v.obj->buckets = calloc(1, sizeof(c->v.obj->buckets) * c->v.obj->alloc_size);

			for(long i = 0, j = 0; j < v->v.obj->fill && i < v->v.obj->alloc_size; i++) {
				if(v->v.obj->buckets[i].key) { 
					c->v.obj->buckets[i].key = strdup(v->v.obj->buckets[i].key);	
					c->v.obj->buckets[i].hash = v->v.obj->buckets[i].hash;
					c->v.obj->buckets[i].value = json_deep_copy(v->v.obj->buckets[i].value);
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
		struct json_array_node* fl;

		fl = from->v.arr->head;
		while(fl) {
			json_array_push_tail(into, json_deep_copy(fl->value));
		}

		return;
	}
	
	// disparate or scalar types
	if(into->type != JSON_TYPE_OBJ || from->type != JSON_TYPE_OBJ) {
		
		// clean out into first
		if(into->type == JSON_TYPE_ARRAY) {
			free_array(into->v.arr);
		}
		else if(into->type == JSON_TYPE_OBJ) {
			free_obj(into->v.obj);
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



void spaces(int depth, int w) { return;
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
static void json_obj_to_string(struct json_write_context* sb, struct json_obj* obj);
static void json_arr_to_string(struct json_write_context* sb, struct json_array* arr);

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
			fprintf(stderr, "c_json: Memory allocation failed\n");
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

void json_value_to_string(struct json_write_context* ctx, struct json_value* v) {
	struct json_string_buffer* sb = ctx->sb;
	char* float_format = ctx->fmt.floatFormat ? ctx->fmt.floatFormat : "%f"; 
	char qc;
	
	if(!v) {
		fprintf(stderr, "NULL value passed to %s()\n", __func__);
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
			sb_tail_catf(sb, "%ld", v->v.integer);
			break;
			
		case JSON_TYPE_DOUBLE: 
			sb_tail_catf(sb, float_format, v->v.dbl); // TODO: handle infinity, nan, etc
			break;
			
		case JSON_TYPE_STRING:
			qc = ctx->fmt.useSingleQuotes ? '\'' : '"';
			sb_putc(sb, qc);
			sb_cat_escaped(ctx, v->v.str); 
			sb_putc(sb, qc);
			break;
			
		case JSON_TYPE_OBJ: 
			json_obj_to_string(ctx, v->v.obj);
			break;
			
		case JSON_TYPE_ARRAY: // 6
			json_arr_to_string(ctx, v->v.arr);
			break;
			
		case JSON_TYPE_COMMENT_SINGLE: // TODO: handle linebreaks in the comment string
			sb_tail_catf(sb, "//%s\n", v->v.str);
			break;
			
		case JSON_TYPE_COMMENT_MULTI: // TODO: clean "*/" out of the comment string
			sb_tail_catf(sb, "/* %s */\n", v->v.str);
			break;
			
		default: 
			fprintf(stderr, "c_json: unknown type in json_value_to_string\n");
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


static void json_arr_to_string(struct json_write_context* ctx, struct json_array* arr) {
	struct json_array_node* n;
	struct json_string_buffer* sb = ctx->sb;
	
	int multiline = arr->length >= ctx->fmt.minArraySzExpand;
	
	sb_putc(sb, '[');
	
	if(multiline) sb_putc(sb, '\n');
	
	ctx->depth++;
	
	n = arr->head;
	while(n) {
		
		if(multiline) ctx_indent(ctx);
		
		json_value_to_string(ctx, n->value);
		
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


static void json_obj_to_string(struct json_write_context* ctx, struct json_obj* obj) {
	int i;
	struct json_obj_field* f;
	struct json_string_buffer* sb = ctx->sb;
	
	int multiline = obj->fill >= ctx->fmt.minObjSzExpand;
	int noquotes = ctx->fmt.noQuoteKeys;
	char quoteChar = ctx->fmt.useSingleQuotes ? '\'' : '"';
	
	sb_putc(sb, '{');
	
	if(multiline) sb_putc(sb, '\n');
	
	ctx->depth++;
	
	int n = obj->fill;
	for(i = 0; i < obj->alloc_size; i++) {
		f = &obj->buckets[i];
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
		
		json_value_to_string(ctx, f->value);
		
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

