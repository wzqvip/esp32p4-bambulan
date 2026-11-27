# 在电脑上跑解析器单元测试：make -C test/host IDF_PATH=~/esp/esp-idf
IDF_PATH ?= $(HOME)/esp/esp-idf
CJSON ?= $(IDF_PATH)/components/json/cJSON
SRC := ../../main/bambu
CFLAGS := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g -fsanitize=address,undefined -I$(SRC) -I$(CJSON)

build/test_parse: test_parse.c $(SRC)/bambu_parse.c $(SRC)/bambu_cmd.c
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $^ $(CJSON)/cJSON.c -lm

test: build/test_parse
	./build/test_parse

.DEFAULT_GOAL := test
