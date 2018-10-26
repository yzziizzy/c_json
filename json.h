#ifndef JSON__JSON_H__INCLUDED
#define JSON__JSON_H__INCLUDED


#include <stdarg.h>
#include <stdint.h>

//#define JSON_DEBUG

#ifdef JSON_DEBUG
	#define dbg_printf(args...) printf(args)
#else
	#define dbg_printf(args...) 
	static void nothin(char* x, ...) {};
	#define dbg_printf nothin 
#endif

// the parser currently does not handle comments.
#define JSON_DISCARD_COMMENTS 1

#ifndef JSON_NO_TYPEDEFS
	#define JSON_TD(x) x
	#define JSON_TYPEDEF typedef
#else 
	#define JSON_TD(x) 
#endif


struct json_obj;
struct json_array;



JSON_TYPEDEF enum json_type {
	JSON_TYPE_UNDEFINED = 0,
	JSON_TYPE_NULL,
	JSON_TYPE_INT,
	JSON_TYPE_DOUBLE,
	JSON_TYPE_STRING,
	JSON_TYPE_OBJ,
	JSON_TYPE_ARRAY,
	JSON_TYPE_COMMENT_SINGLE,
	JSON_TYPE_COMMENT_MULTI,
	
	// only used for unpacking to structs
	JSON_TYPE_FLOAT,
	JSON_TYPE_INT8,
	JSON_TYPE_INT16,
	JSON_TYPE_INT32,
	JSON_TYPE_INT64,
	JSON_TYPE_UINT8,
	JSON_TYPE_UINT16,
	JSON_TYPE_UINT32,
	JSON_TYPE_UINT64,
	
	JSON_TYPE_MAXVALUE
} JSON_TD(json_type_e);

#define JSON__type_enum_tail JSON_TYPE_MAXVALUE

JSON_TYPEDEF enum json_error {
	
	JSON_ERROR_NONE = 0,
	JSON_ERROR_OOM,
	
	JSON_LEX_ERROR_NULL_IN_STRING,
	JSON_LEX_ERROR_NULL_BYTE,
	JSON_LEX_ERROR_INVALID_STRING,
	JSON_LEX_ERROR_UNEXPECTED_END_OF_INPUT,
	JSON_LEX_ERROR_INVALID_CHAR,
	
	JSON_PARSER_ERROR_CORRUPT_STACK,
	JSON_PARSER_ERROR_STACK_EXHAUSTED,
	JSON_PARSER_ERROR_UNEXPECTED_EOI,
	
	JSON_ERROR_MAXVALUE
} JSON_TD(json_error_e);

JSON_TYPEDEF struct json_value {
	enum json_type type;
	union {
		int64_t integer;
		double dbl;
		char* str;
		struct json_obj* obj;
		struct json_array* arr;
	} v;
	union {
		int len;
		int base;
	} info;
} JSON_TD(json_value_t);



struct json_obj_field;

JSON_TYPEDEF struct json_obj {
	size_t alloc_size;
	size_t fill;
	struct json_obj_field* buckets; 
} JSON_TD(json_obj_t);



JSON_TYPEDEF struct json_array_node {
	struct json_array_node* next, *prev;
	struct json_value* value;
} JSON_TD(json_array_node_t);

JSON_TYPEDEF struct json_array {
	int length;
	struct json_array_node* head;
	struct json_array_node* tail;
} JSON_TD(json_array_t);

JSON_TYPEDEF struct json_file {
	struct json_value* root;
	
	void* lex_info; // don't poke around in here...
	
	enum json_error error;
	int err_line_num;
	int err_char_num;
	
} JSON_TD(json_file_t);

JSON_TYPEDEF struct json_string_buffer {
	char* buf;
	size_t length;
	size_t alloc;
} JSON_TD(json_string_buffer_t);


int json_as_type(struct json_value* v, enum json_type t, void* out); 
int json_as_int(struct json_value* v, int64_t* out); 
int json_as_double(struct json_value* v, double* out);
int json_as_string(struct json_value* v, char** out);
int json_as_float(struct json_value* v, float* f);

#define JSON_UNPACK(t, f, type) #f, (t), (void*)(&((t)->f)) - (void*)(t), type
int json_obj_unpack_struct(struct json_value* obj, ...);
int json_obj_unpack_string_array(struct json_value* obj, char*** out, size_t* len);




struct json_array* json_array_create();
int json_array_push_tail(struct json_value* arr, struct json_value* val);
int json_array_pop_tail(struct json_value* arr, struct json_value** val);
size_t json_array_length(struct json_value* arr);

struct json_obj* json_obj_create(size_t initial_alloc_size);
int json_obj_get_key(struct json_value* obj, char* key, struct json_value** val);
int json_obj_set_key(struct json_value* obj, char* key, struct json_value* val);
int json_obj_length(struct json_value* obj);

// iteration. no order. results undefined if modified while iterating
// returns 0 when there is none left
// set iter to NULL to start
int json_obj_next(struct json_value* val, void** iter, char** key, struct json_value** value);

struct json_file* json_load_path(char* path);
struct json_file* json_read_file(FILE* f);
struct json_file* json_parse_string(char* source, size_t len);

// recursive.
void json_free(struct json_value* v);
// TODO: json_file_free(struct json_file* jsf);

char* json_get_type_str(enum json_type t); 
char* json_get_err_str(enum json_error e);

void json_dump_value(struct json_value* root, int cur_depth, int max_depth);

#endif // JSON__JSON_H__INCLUDED
