#!/bin/bash


gcc -o cjson main.c json.c MurmurHash3.c -lm -ggdb -std=c11 \
	-Wno-implicit-function-declaration \
	-fstrict-aliasing 

 valgrind --leak-check=full --show-leak-kinds=all --error-limit=no --track-origins=yes ./cjson $@

