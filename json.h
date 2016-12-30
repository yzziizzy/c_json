

#define dbg_printf printf


#define JSON_DISCARD_COMMENTS 1


struct json_obj;
struct json_array;


enum json_type {
	JSON_TYPE_UNDEFINED = 0,
	JSON_TYPE_NULL,
	JSON_TYPE_INT,
	JSON_TYPE_DOUBLE,
	JSON_TYPE_STRING,
	JSON_TYPE_OBJ,
	JSON_TYPE_ARRAY,
	JSON_TYPE_COMMENT_SINGLE,
	JSON_TYPE_COMMENT_MULTI
};

enum json_error {
	
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
};

struct json_value{
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
};

struct json_obj_field {
	uint64_t hash;
	char* key;
	struct json_value* value;
};


struct json_obj {
	size_t alloc_size;
	size_t fill;
	struct json_obj_field* buckets; 
};




struct json_array_node {
	struct json_array_node* next, *prev;
	struct json_value* value;
};

struct json_array {
	int length;
	struct json_array_node* head;
	struct json_array_node* tail;
};

struct json_file {
	struct json_value* root;
	
	void* lex_info; // don't poke around in here...
	
	enum json_error error;
	int err_line_num;
	int err_char_num;
	
};








