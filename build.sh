#!/bin/bash


gcc -o cjson main.c json.c MurmurHash3.c -lm -ggdb -std=c11 \
	-Wno-implicit-function-declaration \
	-fstrict-aliasing 


