

#define dbg_printf printf

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
	JSON_TYPE_COMMENT_MULTI
} JSON_TD(json_type_e);

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
	JSON_PARSER_ERROR_UNEXPECTED_EOI
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



struct json_array* json_array_create();
int json_array_push_tail(struct json_value* arr, struct json_value* val);
int json_array_pop_tail(struct json_value* arr, struct json_value** val);

struct json_obj* json_obj_create(size_t initial_alloc_size);
int json_obj_get_key(struct json_value* obj, char* key, struct json_value** val);
int json_obj_set_key(struct json_value* obj, char* key, struct json_value* val);


struct json_file* json_load_path(char* path);
struct json_file* json_read_file(FILE* f);
struct json_file* json_parse_string(char* source, size_t len);


char* json_get_type_str(enum json_type t); 
char* json_get_err_str(enum json_error e);
