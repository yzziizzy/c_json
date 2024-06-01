#ifndef JSON__JSON_H__INCLUDED
#define JSON__JSON_H__INCLUDED


#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

//#define JSON_DEBUG

#ifdef JSON_DEBUG
	#define dbg_printf(args...) printf(args)
#else
	#define dbg_printf(args...) {}
	//static void nothin(char* x, ...) {};
	//efine dbg_printf nothin 
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
	JSON_TYPE_BOOL,
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
	JSON_PARSER_ERROR_UNEXPECTED_TOKEN,
	JSON_PARSER_ERROR_BRACE_MISMATCH,
	JSON_PARSER_ERROR_BRACKET_MISMATCH,
	
	JSON_ERROR_MAXVALUE
} JSON_TD(json_error_e);


struct json_obj_field;
struct json_link;

JSON_TYPEDEF struct json_value {
	enum json_type type;
	int base;
	
	size_t len;
	
	union {
		int64_t n;
		uint64_t u;
		double d;
		char* s;
		struct {
			size_t alloc_size;
			struct json_obj_field* buckets; 
		} obj;
		struct {
			struct json_link* head, *tail;
		} arr;
	};
} JSON_TD(json_value_t);




JSON_TYPEDEF struct json_link {
	struct json_link* next, *prev;
	struct json_value* v;
} JSON_TD(json_link_t);



JSON_TYPEDEF struct json_file {
	struct json_value* root;
	
	void* lex_info; // don't poke around in here...
	
	enum json_error error;
	char* error_str;
	long error_line_num;
	long error_char_num;
	
} JSON_TD(json_file_t);


JSON_TYPEDEF struct json_string_buffer {
	char* buf;
	size_t length;
	size_t alloc;
	int line_len;
} JSON_TD(json_string_buffer_t);


JSON_TYPEDEF struct json_output_format {
	char indentChar;
	char indentAmt;
	char trailingComma;
// 	char commaBeforeElem;
	char objColonSpace;
	char noQuoteKeys;
	char useSingleQuotes;
// 	char escapeNonLatin;
// 	char breakLongStrings;
	int minArraySzExpand;
	int minObjSzExpand;
	int maxLineLength; // only wraps after the comma on array/obj elements
	char* floatFormat;
} JSON_TD(json_output_format_t);


JSON_TYPEDEF struct json_write_context {
	int depth;
	struct json_string_buffer* sb;
	struct json_output_format fmt;
} JSON_TD(json_write_context_t);



/*
Type coercion rules:
	undefined/null -> int = 0
	numbers, as you would expect, according to C type conversion rules
	obj/array -> number/string = error
	string/comment -> string = string
	number -> string = sprintf, accoding to some nice rules.
	string -> number = strtod/i
	
Strings are always dup'd. You must free them yourself.

JSON_TYPE_INT is assumed to be C int

Numbers over 2^63 are not properly supported yet. they will be truncated to 0
*/
int json_as_type(struct json_value* v, enum json_type t, void* out); 
int64_t json_as_int(struct json_value* v); 
double  json_as_double(struct json_value* v);
char*   json_as_strdup(struct json_value* v);
float   json_as_float(struct json_value* v);

/*
#define JSON_UNPACK(t, f, type) #f, (t), (void*)(&((t)->f)) - (void*)(t), type
int json_obj_unpack_struct(int count, struct json_value* obj, ...);
int json_obj_unpack_string_array(struct json_value* obj, char*** out, size_t* len);
*/


int json_array_push_tail(struct json_value* arr, struct json_value* val);
int json_array_push_head(struct json_value* arr, struct json_value* val);
struct json_value* json_array_pop_tail(struct json_value* arr);
struct json_value* json_array_pop_head(struct json_value* arr);

// USUALLY UNNECESSARY. manually calculate the array length.
size_t json_array_calc_length(struct json_value* arr);

int json_obj_get_key(struct json_value* obj, char* key, struct json_value** val);
int json_obj_set_key(struct json_value* obj, char* key, struct json_value* val);
int json_obj_set_key_nodup(struct json_value* obj, char* key, struct json_value* val); // takes ownership of key's memory

// will probably be changed or removed later
// coerces and strdup's the result
// returns null if the key does not exist
char* json_obj_key_as_string(struct json_value* obj, char* key);

// returns a newly allocated string, or NULL if it's not a string
char* json_obj_get_strdup(struct json_value* obj, char* key);

// returns pointer to the internal string, or null if it's not a string
char* json_obj_get_str(struct json_value* obj, char* key);

// returns a double or the default value if it's not an integer
double json_obj_get_double(struct json_value* obj, char* key, double def);

// returns an integer or the default value if it's not an integer
int64_t json_obj_get_int(struct json_value* obj, char* key, int64_t def);

// returns the json_value strut for a key, or null if it doesn't exist
struct json_value* json_obj_get_val(struct json_value* obj, char* key);



// iteration. no order. results undefined if modified while iterating
// returns 0 when there is none left
// set iter to NULL to start
int json_obj_next(struct json_value* val, void** iter, char** key, struct json_value** value);

struct json_value* json_deep_copy(struct json_value* v);
void json_merge(struct json_value* into, struct json_value* from); 

struct json_file* json_load_path(char* path);
struct json_file* json_read_file(FILE* f);
struct json_file* json_parse_string(char* source, size_t len);

// recursive.
void json_free(struct json_value* v);
void json_file_free(struct json_file* jsf);

struct json_value* json_new_str(char* s);
struct json_value* json_new_strn(char* s, size_t len);
struct json_value* json_new_double(double d);
struct json_value* json_new_int(int64_t n);
struct json_value* json_new_array(void);
struct json_value* json_new_object(size_t initial_alloc_size);
struct json_value* json_new_null(void);
struct json_value* json_new_undefined(void);
struct json_value* json_new_true(void);
struct json_value* json_new_false(void);




char* json_get_type_str(enum json_type t); 
char* json_get_err_str(enum json_error e);

void json_dump_value(struct json_value* root, int cur_depth, int max_depth);



struct json_string_buffer* json_string_buffer_create(size_t initSize);
void json_string_buffer_free(struct json_string_buffer* sb);

void json_stringify(struct json_write_context* ctx, struct json_value* v);




/*
Loop macro magic

https://www.chiark.greenend.org.uk/~sgtatham/mp/

json_value_t* obj;
JSON_OBJ_EACH(&obj, key, val) {
	printf("loop: %s, %s", key, val->s);
}

effective source:

	#define JSON_OBJ_EACH(obj, keyname, valname)
	if(0)
		finished: ;
	else
		for(char* keyname;;) // internal declarations, multiple loops to avoid comma op funny business
		for(valtype valname;;)
		for(void* iter = NULL ;;)
			if(json_obj_next(obj, iter, &keyname, &valname))
				goto main_loop;
			else
				while(1)
					if(1) {
						// when the user uses break
						goto finished;
					}
					else
						while(1)
							if(!json_obj_next(obj, iter, &keyname, &valname)) {
								// normal termination
								goto finished;
							}
							else
								main_loop:
								//	{ user block; not in macro }
*/
#define JSONHASH__PASTEINNER(a, b) a ## b
#define JSONHASH__PASTE(a, b) JSONHASH__PASTEINNER(a, b) 
#define JSONHASH__ITER(key, val) JSONHASH__PASTE(jsonhashtable_iter_ ## key ## __ ## val ## __, __LINE__)
#define JSONHASH__FINISHED(key, val) JSONHASH__PASTE(jsonhashtable_finished__ ## key ## __ ## val ## __, __LINE__)
#define JSONHASH__MAINLOOP(key, val) JSONHASH__PASTE(jsonhashtable_main_loop__ ## key ## __ ## val ## __, __LINE__)    
#define JSON_OBJ_EACH(obj, keyname, valname) \
if(0) \
	JSONHASH__FINISHED(keyname, val): ; \
else \
	for(char* keyname ;;) \
	for(struct json_value* valname ;;) \
	for(void* JSONHASH__ITER(keyname, val) = NULL ;;) \
		if(json_obj_next(obj, & (JSONHASH__ITER(keyname, val)), &keyname, &valname)) \
			goto JSONHASH__MAINLOOP(keyname, val); \
		else \
			while(1) \
				if(1) { \
					goto JSONHASH__FINISHED(keyname, val); \
				} \
				else \
					while(1) \
						if(!json_obj_next(obj, & (JSONHASH__ITER(keyname, val)), &keyname, &valname)) { \
							goto JSONHASH__FINISHED(keyname, val); \
						} \
						else \
							JSONHASH__MAINLOOP(keyname, val) :
							
							//	{ user block; not in macro }








#endif // JSON__JSON_H__INCLUDED
