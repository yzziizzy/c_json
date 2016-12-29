

#define dbg_printf printf


#define JSON_DISCARD_COMMENTS


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
	size_t allocSize;
	size_t fill;
	struct json_obj_field* buckets; 
};




struct json_array_node {
	struct json_array_node* next;
	struct json_value* value;
};

struct json_array {
	int length;
	struct json_array_node* head;
	struct json_array_node* tail;
};









