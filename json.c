 
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "json.h"

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


struct json_array* json_create_array() {

	struct json_array* arr;
	
	arr = malloc(sizeof(*arr));
	if(!arr) return NULL;
	
	arr->length = 0;
	arr->head = NULL;
	arr->tail = NULL;
	
	return arr;
}


// pushes the tail
int json_array_push_tail(struct json_array* arr, struct json_value* val) {

	struct json_array_node* node;
	
	node = malloc(sizeof(*node));
	if(!node) return 1;
	
	node->next = NULL;
	node->prev = arr->tail;
	node->value = val;
	
	if(arr->length == 0) {
		arr->head = node;
	}
	else {
		arr->tail->next = node;
	}
	
	arr->tail = node;
	arr->length++;
	
	return 0;
}

// pops the tail
int json_array_pop_tail(struct json_array* arr, struct json_value** val) {

	struct json_array_node* t;
	
	if(arr->length == 0) {
		return 1;
	}
	
	arr->length--;
	
	*val = arr->tail->value;
	
	if(arr->length > 0) {
		t = arr->tail;
		arr->tail = arr->tail->prev;
		arr->tail->next = NULL;
	}
	else {
		arr->head = arr->tail = NULL;
	}
	
	free(t);
	
	return 0;
}

struct json_obj* json_create_obj(size_t initial_alloc_size) {
	
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

static size_t find_bucket(struct json_obj* obj, uint64_t hash, char* key) {
	size_t startBucket, bi;
	
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
	
	// should never reach here if the table is maintained properly
	return -1;
}


// should always be called with a power of two
static int json_obj_resize(struct json_obj* obj, int newSize) {
	struct json_obj_field* old, *op;
	size_t oldlen = obj->alloc_size;
	size_t i, n, bi;
	
	old = op = obj->buckets;
	
	obj->alloc_size = newSize;
	obj->buckets = calloc(1, sizeof(*obj->buckets) * newSize);
	if(!obj->buckets) return 1;
	
	for(i = 0, n = 0; i < oldlen && n < obj->fill; i++) {
		if(op->key == NULL) continue;
		
		bi = find_bucket(obj, op->hash, op->key);
		obj->buckets[bi].value = op->value;
		obj->buckets[bi].hash = op->hash;
		obj->buckets[bi].key = op->key;
		
		n++;
	}
	
	free(old);
	
	return 0;
}


// TODO: better return values and missing key handling
// returns 0 if val is set to the value
// *val == NULL && return 0  means the key was not found;
int json_obj_get_key(struct json_obj* obj, char* key, struct json_value** val) {
	uint64_t hash;
	size_t bi;
	
	hash = hash_key(key, -1);
	
	bi = find_bucket(obj, hash, key);
	if(bi < 0) return 1;
	
	*val = obj->buckets[bi].value; 
	return 0;
}

// zero for success
int json_obj_set_key(struct json_obj* obj, char* key, struct json_value* val) {
	uint64_t hash;
	size_t bi;
	
	// check size and grow if necessary
	if(obj->fill / obj->alloc_size >= 0.75) {
		json_obj_resize(obj, obj->alloc_size * 2);
	}
	
	hash = hash_key(key, -1);
	
	bi = find_bucket(obj, hash, key);
	if(bi < 0) return 1;
	
	obj->buckets[bi].value = val;
	
	return 0;
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
	
	printf("string \n");
	//printf("\n\ndelim %c\n", delim);
	//printf("error: %d\n", jl->error);
	// find len, count lines
	while(1) {
		if(*se == delim) break;
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
	
	printf("number \n");
	
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

	printf("label \n");
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
	
	printf("comment \n");
	
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
	
	arr = json_create_array();
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
	
	obj = json_create_obj(4);
	
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
	
	printf("ct-%d-", jp->cur_token[1].line_num); dbg_print_token(jp->cur_token + 1);
	
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
	json_array_push_tail(arr->v.arr, v);
	
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
	json_obj_set_key(obj->v.obj, l->v.str, v);
	// BUG? free label value?
	
	jp->stack_cnt -= 2;
}


static struct json_value* parse_token_stream(struct json_lexer* jl) {
	
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
		dbg_printf("\nparse_array\n");
		
		if(jp->cur_token >= jp->last_token) goto CHECK_END;
	
		// cycle: val, comma
		switch(tok->tokenType) {
		
			case TOKEN_ARRAY_START: dbg_printf("TOKEN_ARRAY_START\n");
				parser_push(jp, RESUME_ARRAY);
				parser_push_new_array(jp);
				next();
				goto PARSE_ARRAY;
				
			case TOKEN_ARRAY_END: dbg_printf("TOKEN_ARRAY_END\n");
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
			
			case TOKEN_OBJ_START: dbg_printf("PARSE_OBJ\n");
				parser_push(jp, RESUME_ARRAY);
				parser_push_new_object(jp);
				next();
				goto PARSE_OBJ;
				
			case TOKEN_STRING:
			case TOKEN_NUMBER:
			case TOKEN_NULL:
			case TOKEN_UNDEFINED: dbg_printf("PARSE VALUE \n");
				parser_push(jp, tok->val);
				reduce_array(jp);
				
				consume_commas(jp);
				next();
				
				goto PARSE_ARRAY;
		
			case TOKEN_OBJ_END: dbg_printf("BRACKET_MISMATCH\n");
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
		dbg_printf("\nparse_obj\n");
	// cycle: label, colon, val, comma
	
		if(tok->tokenType == TOKEN_OBJ_END) {
			dbg_printf("obj- TOKEN_OBJ_END\n");
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
		else if(tok->tokenType != TOKEN_LABEL) {
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
		
			case TOKEN_ARRAY_START: dbg_printf("obj- TOKEN_ARRAY_START\n");
				parser_push(jp, RESUME_OBJ);
				parser_push_new_array(jp);
				next();
				goto PARSE_ARRAY;
				
			case TOKEN_OBJ_END: dbg_printf("obj- !!!TOKEN_OBJ_END in value slot\n");

				
				 dbg_printf("!!! escaped sentinel block\n");
				break;
			
			case TOKEN_OBJ_START: dbg_printf("obj- TOKEN_OBJ_START\n");
				parser_push(jp, RESUME_OBJ);
				parser_push_new_object(jp); // BUG
				next();
				goto PARSE_OBJ;
				
			case TOKEN_STRING:
			case TOKEN_NUMBER:
			case TOKEN_NULL:
			case TOKEN_UNDEFINED: dbg_printf("obj- TOKEN VALUE\n");
				parser_push(jp, tok->val);
				reduce_object(jp);
				
				consume_commas(jp);
				next();
				
				goto PARSE_OBJ;
		
			case TOKEN_ARRAY_END: dbg_printf("obj- BRACE_MISMATCH\n");
				goto BRACE_MISMATCH;
			
			case TOKEN_NONE:
			case TOKEN_LABEL:
			case TOKEN_COMMA:
			case TOKEN_COLON:
			default: dbg_printf("obj- UNEXPECTED_TOKEN\n");
				// invalid
				goto UNEXPECTED_TOKEN;
		}
	
	dbg_printf("end of obj\n");

	return NULL;
CHECK_END:  dbg_printf("!!! CHECK_END\n");
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
	struct json_lexer* jl;
	struct json_parser* jp;
	size_t nr;
	
	// check file size
	fseek(f, 0, SEEK_END);
	fsz = ftell(f);
	fseek(f, 0, SEEK_SET);
	
	contents = malloc(fsz+1);
	contents[fsz] = 0; // some crt functions might read past the end otherwise
	
	nr = fread(contents, 1, fsz, f);

	jl = tokenize_string(contents, fsz);
	
	free(contents);
	
	if(!jl) return NULL;
	
	jp = parse_token_stream(jl);

	return jp->stack + 1;
}

int main(int argc, char* argv[]) {
	FILE* f;
	size_t fsz;
	char* contents, *txt;
	int nr;
	struct json_file* jf;
	
	jf = json_load_path(argv[1]);
// 	f = fopen(argv[1], "rb");
// 	if(!f) {
// 		printf("no such file\n");
// 		return 1;
// 	}
// 	
// 	fseek(f, 0, SEEK_END);
// 	
// 	fsz = ftell(f);
// 	dbg_printf("file size: %d\n", (int)fsz);
// 	
// 	fseek(f, 0, SEEK_SET);
// 	
// 	txt = contents = malloc(fsz+1);
// 	txt[fsz] = 0; // some crt functions might read past the end otherwise
// 	
// 	nr = fread(contents, 1, fsz, f);
// 	dbg_printf("bytes read: %d\n", nr);
// 	fclose(f);
// 	
// 	struct json_lexer* jl = tokenize_string(txt, fsz);
// 	
// 	struct token* ts = jl->token_stream;
// 	
// #define tc(x, y, z) case x: printf(#x ": " y "\n", z); break;
// #define tcl(x) case x: printf(#x "\n"); break;
// 	printf("%d,%d\n", jl->ts_cnt, jl->ts_alloc);
// 	//exit(1);
// 	int i;
// 	for(i = 0; i < jl->ts_cnt; i++, ts++) {
// 		dbg_print_token(ts);
// 	}
// 	
// 	printf("\n----------------\n\n");
// 	
// 	parse_token_stream(jl);
// 	
// 	free(contents);
// 	
// 	
	return 0;
}


#define tc(x, y, z) case x: printf(#x ": " y "\n", z); break;
#define tcl(x) case x: printf(#x "\n"); break;

static void dbg_print_token(struct token* ts) {
	switch(ts->tokenType) {
		tcl(TOKEN_NONE)
		tcl(TOKEN_ARRAY_START)
		tcl(TOKEN_ARRAY_END)
		tcl(TOKEN_OBJ_START)
		tcl(TOKEN_OBJ_END)
		tc(TOKEN_STRING, "%s", ts->val->v.str)
		tc(TOKEN_NUMBER, "%d", ts->val->v.integer)
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
		printf("ROOT_VALUE sentinel\n");
		return;
	}
	if(v == RESUME_ARRAY) {
		printf("RESUME_ARRAY sentinel\n");
		return;
	}
	if(v == RESUME_OBJ) {
		printf("RESUME_OBJ sentinel\n");
		return;
	}
	
	switch(v->type) {
		case JSON_TYPE_UNDEFINED: printf("undefined\n"); break;
		case JSON_TYPE_NULL: printf("null\n"); break;
		case JSON_TYPE_INT: printf("int: %d\n", v->v.integer); break;
		case JSON_TYPE_DOUBLE: printf("double %f\n", v->v.dbl); break;
		case JSON_TYPE_STRING: printf("string: \"%s\"\n", v->v.str); break;
		case JSON_TYPE_OBJ: printf("object [%d]\n", v->v.obj->fill); break;
		case JSON_TYPE_ARRAY: printf("array [%d]\n", v->v.arr->length); break;
		case JSON_TYPE_COMMENT_SINGLE: printf("comment, single\n"); break;
		case JSON_TYPE_COMMENT_MULTI: printf("comment, multiline\n"); break;
	}
}

static void dbg_dump_stack(struct json_parser* jp, int depth) {
	int i = 0;
	
	for(i = 0; i < depth && i < jp->stack_cnt; i++) {
		printf("%d: ", i);
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
		case JSON_TYPE_COMMENT_MULTI: "multi-line comment";
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
		default: "Invalid Error Code";
	}
}
	


