 
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "json.h"


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

enum lex_error {
	LEX_ERROR_NONE = 0,
	LEX_ERROR_OOM,
	LEX_ERROR_NULL_IN_STRING,
	LEX_ERROR_NULL_BYTE,
	LEX_ERROR_INVALID_STRING,
	LEX_ERROR_UNEXPECTED_END_OF_INPUT,
	LEX_ERROR_INVALID_CHAR
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

#define check_oom(x) \
if(x == NULL) { \
	jl->error = LEX_ERROR_OOM; \
	return 1; \
}

// out must be big enough, at least as big as in+1 just to be safe
// appends a null to out, but is also null-safe
int decode_c_escape_str(char* in, char* out, size_t len) {
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
void lex_next_char(struct json_lexer* jl) {
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


// returns error code. val set to null
int lex_push_token(struct json_lexer* jl, enum token_type t) {
	return lex_push_token_val(jl, t, NULL);
}

// returns erro code.
int lex_push_token_val(struct json_lexer* jl, enum token_type t, struct json_value* val) {
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
int count = 0;

int lex_string_token(struct json_lexer* jl) {
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
			jl->error = LEX_ERROR_NULL_IN_STRING;
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
			jl->error = LEX_ERROR_UNEXPECTED_END_OF_INPUT;
			return 1;
		}
	}
	//printf("error: %d\n", jl->error);
	
	len = se - jl->head - 1;
	
	str = malloc(len+1);
	check_oom(str)
	
	//printf("error: %d\n", jl->error);
	
	if(decode_c_escape_str(jl->head + 1, str, len)) {
		jl->error = LEX_ERROR_INVALID_STRING;
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


int lex_number_token(struct json_lexer* jl) {
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


int lex_label_token(struct json_lexer* jl) {
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
			jl->error = LEX_ERROR_UNEXPECTED_END_OF_INPUT;
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



int lex_comment_token(struct json_lexer* jl) {
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
				jl->error = LEX_ERROR_NULL_BYTE;
				return 1;
			}
			
			char_num++;
			se++;
			
			if(se > jl->end) {
				jl->error = LEX_ERROR_UNEXPECTED_END_OF_INPUT;
				return 1;
			}
		}
	}
	else if(delim == '*') { // multline
		// find len, count lines
		while(1) {
			if(se[0] == '*' && se[1] == '/') break;
			if(*se == '\0') {
				jl->error = LEX_ERROR_NULL_BYTE;
				return 1;
			}
			
			if(*se == '\n') {
				lines++;
				char_num = 1;
			}
			char_num++;
			se++;
			
			if(se > jl->end) {
				jl->error = LEX_ERROR_UNEXPECTED_END_OF_INPUT;
				return 1;
			}
		}
	}
	else {
		jl->error = LEX_ERROR_INVALID_CHAR;
		return 1;
	}
	
	len = se - jl->head - 1;
	
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
int lex_nibble(struct json_lexer* jl) {
	
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
			jl->error = LEX_ERROR_INVALID_CHAR;
			return 1;
	}
	
	//printf("lol\n");
	
	lex_next_char(jl);
	
	return jl->error || jl->head >= jl->end;
}


struct json_lexer* tokenize_string(char* source, size_t len) {
	
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



void parser_push(struct json_parser* jp, struct json_value* v) {
	void* tmp;
	
	int alloc = jp->stack_alloc;
	int cnt = jp->stack_cnt;
	
	// check size
	if(cnt >= alloc) {
		if(alloc == 0) alloc = 16;
		alloc *= 2;
		tmp = realloc(jp->stack, alloc * sizeof(*(jp->stack)));
		if(!tmp) {
			jp->error = 1;
			return;
		}
		
		jp->stack = tmp;
		jp->stack_alloc = alloc;
	}
	
	jp->stack[cnt] = v;
	jp->stack_cnt++;
}

struct json_value* parser_pop(struct json_parser* jp) {
	
	if(jp->stack_cnt <= 0) {
		jp->error = 2;
		return NULL;
	}
	
	return jp->stack[--jp->stack_cnt];
}

void parser_push_new_array(struct json_parser* jp) {
	
	struct json_value* val;
	struct json_array* arr;
	
	val = malloc(sizeof(*val));
	if(!val) {
		jp->error = 5;
		return;
	}
	
	arr = malloc(sizeof(*arr));
	if(!arr) {
		jp->error = 5;
		return;
	}
	
	arr->head = NULL;
	arr->tail = NULL;
	arr->length = 0;
	
	val->type = JSON_TYPE_ARRAY;
	val->v.arr = arr;
	
	parser_push(jp, val);
}

// push items into the array on the top of the stack
void parser_push_array(struct json_parser* jp, struct json_value* val) {
	
	// append to the array;
	
}

struct token* consume_token(struct json_parser* jp) {
	if(jp->cur_token > jp->last_token) {
		jp->error = 3;
		return NULL;
	}
	
	return ++jp->cur_token;
}

void consume_comments(struct json_parser* jp) {
	while(1) {
		if(jp->cur_token->tokenType != TOKEN_COMMENT) break;
		
		parser_push(jp, jp->cur_token->val);
		consume_token(jp);
	}
}

// compact the stack
void reduce_array(struct json_parser* jp) {
	
	
}

struct json_value* parse_token_stream(struct json_lexer* jl) {
	
	int i;
	struct json_parser* jp;
	struct token* tok;
	
	jp = malloc(sizeof(*jp));
	if(!jp) {
		return NULL;
	}
	
	tok = jl->token_stream;
	jp->token_stream = jl->token_stream;
	jp->cur_token = jl->token_stream;
	jp->last_token = jl->token_stream + jl->ts_cnt;

	i = 0;

#define next() if(!(tok = consume_token(jp))) goto UNEXPECTED_EOI;

	consume_comments(jp);

	PARSE_ARRAY:

		consume_comments(jp); // BUG need macro here to advance tok if it changed
		
		// cycle: val, comma
		switch(tok->tokenType) {
		
			case TOKEN_ARRAY_START:
				parser_push_val(jp, RESUME_ARRAY);
				parser_push_new_array(jp);
				goto PARSE_ARRAY;
				
			case TOKEN_ARRAY_END:
				// pop from the stack until array start token found
			
			case TOKEN_OBJ_START:
				parser_push_val(jp, RESUME_ARRAY);
				parser_push_new_array(jp);
				goto PARSE_OBJ;
				
			case TOKEN_STRING:
			case TOKEN_NUMBER:
			case TOKEN_NULL:
			case TOKEN_UNDEFINED:
				parser_push_val(jp, tok->val);
				next();
				reduce_array(jp);
				
				goto PARSE_ARRAY;
		
			case TOKEN_OBJ_END:
				goto BRACKET_MISMATCH;
			
			case TOKEN_NONE:
			case TOKEN_LABEL:
			case TOKEN_COMMA:
			case TOKEN_COLON:
			default:
				// invalid
				goto UNEXPECTED_TOKEN;
		}
	
	
PARSE_OBJ:
	for(; i < jl->ts_cnt; i++, tok++) {
		// cycle: label, colon, value
		if(tok[0].tokenType != TOKEN_LABEL) {
			// parse error
		}
		if(tok[1].tokenType != TOKEN_COLON) {
			// parse error
		}
		
		
	
	}

	return NULL;
END:
	return NULL;
UNEXPECTED_EOI: // end of input
	return NULL;
UNEXPECTED_TOKEN:
	return NULL;
BRACKET_MISMATCH:
	return NULL;
}

int main(int argc, char* argv[]) {
	FILE* f;
	size_t fsz;
	char* contents, *txt;
	int nr;
	
	f = fopen(argv[1], "rb");
	if(!f) {
		printf("no such file\n");
		return 1;
	}
	
	fseek(f, 0, SEEK_END);
	
	fsz = ftell(f);
	dbg_printf("file size: %d\n", (int)fsz);
	
	fseek(f, 0, SEEK_SET);
	
	txt = contents = malloc(fsz+1);
	txt[fsz] = 0; // some crt functions might read past the end otherwise
	
	nr = fread(contents, 1, fsz, f);
	dbg_printf("bytes read: %d\n", nr);
	fclose(f);
	
	struct json_lexer* jl = tokenize_string(txt, fsz);
	
	struct token* ts = jl->token_stream;
	
#define tc(x, y, z) case x: printf(#x ": " y "\n", z); break;
#define tcl(x) case x: printf(#x "\n"); break;
	printf("%d,%d\n", jl->ts_cnt, jl->ts_alloc);
	//exit(1);
	int i;
	for(i = 0; i < jl->ts_cnt; i++, ts++) {
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
	
	
	free(contents);
	
	
	return 0;
}










