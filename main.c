
  
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "json.h"

int main(int argc, char* argv[]) {
	FILE* f;
	size_t fsz;
	char* contents, *txt;
	int nr;
	struct json_file* jf;
	
	jf = json_load_path("./test.json");//argv[1]);
	
	struct json_output_format fmt = {
		.indentChar = ' ',
		.indentAmt = 4,
		.minArraySzExpand = 4,
		.minObjSzExpand = 3,
		.trailingComma = 1,
		.objColonSpace = 1,
		.useSingleQuotes = 1,
		.noQuoteKeys = 1,
		.maxLineLength = 20,
	};
	
	
	struct json_write_context ctx;
	ctx.sb = json_string_buffer_create(4000);
	ctx.fmt = fmt;
	ctx.depth = 0;
	
	json_value_to_string(&ctx, jf->root);
// 	json_value_to_string(&ctx, NULL);
	
	
	printf(">%s<\n", ctx.sb->buf);
	
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
//	getchar();
	return 0;
}
