# sqedit's own build.
#
#   make CXX=/path/to/clang++   clang and libc++ 19 or later (23 is what
#                                sqterm and CI use), for `import std;`
#   make test                    the engine's tests, then tests/run_check.sh
#
# Offline, and with no sed, grep or awk: sqedit is built before anything
# uses it, on a machine that may have none of them (its design,
# "Bootstrapping"). The std module is built here from the libc++ the
# compiler uses; sq.data (squared's JSON module) and yyjson are vendored.

ifeq ($(origin CXX),default)
CXX      := clang++
endif
BUILD    ?= build
OPT      ?= -O2 -g
STDLIB   := -stdlib=libc++
WARN     := -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion -Wno-missing-field-initializers \
            -Wno-missing-designated-field-initializers
CXXFLAGS := -std=c++23 $(STDLIB) $(OPT) $(WARN) -isystem third_party/yyjson-0.12.0/src \
            -fprebuilt-module-path=$(BUILD)/pcm -fmodule-file=std=$(BUILD)/pcm/std.pcm
CFLAGS   := $(OPT) -w
LDFLAGS  := $(STDLIB)

# libc++'s std module, as its manifest names it ("source-path", relative to
# the manifest), read with make's own functions: no sed. Set STD_CPPM when
# it is somewhere else.
comma        := ,
STD_MANIFEST := $(shell readlink -f "$$($(CXX) $(STDLIB) -print-library-module-manifest-path 2>/dev/null)" 2>/dev/null)
STD_WORDS    := $(if $(wildcard $(STD_MANIFEST)),$(subst ", ,$(subst $(comma), ,$(file < $(STD_MANIFEST)))))
STD_SOURCE   := $(firstword $(filter %/std.cppm std.cppm,$(STD_WORDS)))
STD_CPPM     ?= $(if $(filter /%,$(STD_SOURCE)),$(STD_SOURCE),$(if $(STD_SOURCE),$(dir $(STD_MANIFEST))$(STD_SOURCE)))

# squared's data module (sq.data, vendored: tools/vendor_squared_data.sh),
# its units in the order they import each other (file:module name), then
# its implementation units; and yyjson, which only it uses.
SQD       := third_party/squared-data
SQD_UNITS := src/json_error_code:src.json_error_code src/json_error:src.json_error \
             src/json_parse_options:src.json_parse_options json_value:json_value \
             src/json_parse_result:src.json_parse_result src/json_write_options:src.json_write_options \
             src/json_write_result:src.json_write_result json:json
sqd_name   = $(lastword $(subst :, ,$(1)))
SQD_PCMS  := $(foreach u,$(SQD_UNITS),$(BUILD)/pcm/sq.data-$(call sqd_name,$(u)).pcm)
SQD_OBJS  := $(foreach u,$(SQD_UNITS),$(BUILD)/obj/sq.data/$(call sqd_name,$(u)).o) \
             $(BUILD)/obj/sq.data/module.o $(BUILD)/obj/sq.data/json.cpp.o $(BUILD)/obj/sq.data/json_value.cpp.o

LIB     := $(BUILD)/libsqedit.a
OBJS    := $(BUILD)/obj/std.o $(BUILD)/obj/peg.o $(BUILD)/obj/grammars.o $(SQD_OBJS) $(BUILD)/obj/yyjson.o
TESTS   := $(patsubst tests/%.cpp,$(BUILD)/%,$(wildcard tests/test_*.cpp))

.PHONY: all test clean
all: $(BUILD)/sqedit $(TESTS)

$(BUILD)/pcm/std.pcm:
	@test -n "$(STD_CPPM)" && test -f "$(STD_CPPM)" || { echo "no libc++ std module (set STD_CPPM)" >&2; exit 1; }
	@mkdir -p $(BUILD)/pcm $(BUILD)/obj
	$(CXX) -std=c++23 $(STDLIB) $(OPT) -Wno-reserved-module-identifier --precompile $(STD_CPPM) -o $@
$(BUILD)/obj/std.o: $(BUILD)/pcm/std.pcm
	$(CXX) $(OPT) -c $< -o $@

define sqd_unit
$(BUILD)/pcm/sq.data-$(2).pcm $(BUILD)/obj/sq.data/$(2).o &: $(SQD)/$(1).cppm $(3)
	@mkdir -p $(BUILD)/pcm $(BUILD)/obj/sq.data
	$$(CXX) $$(CXXFLAGS) -w -c $$< -fmodule-output=$(BUILD)/pcm/sq.data-$(2).pcm -o $(BUILD)/obj/sq.data/$(2).o
endef
SQDP := $(BUILD)/pcm/sq.data-
$(eval $(call sqd_unit,src/json_error_code,src.json_error_code,$(BUILD)/pcm/std.pcm))
$(eval $(call sqd_unit,src/json_error,src.json_error,$(SQDP)src.json_error_code.pcm))
$(eval $(call sqd_unit,src/json_parse_options,src.json_parse_options,$(SQDP)src.json_error.pcm))
$(eval $(call sqd_unit,json_value,json_value,$(SQDP)src.json_parse_options.pcm))
$(eval $(call sqd_unit,src/json_parse_result,src.json_parse_result,$(SQDP)json_value.pcm))
$(eval $(call sqd_unit,src/json_write_options,src.json_write_options,$(SQDP)src.json_parse_result.pcm))
$(eval $(call sqd_unit,src/json_write_result,src.json_write_result,$(SQDP)src.json_write_options.pcm))
$(eval $(call sqd_unit,json,json,$(SQDP)src.json_write_result.pcm))
$(BUILD)/pcm/sq.data.pcm $(BUILD)/obj/sq.data/module.o &: $(SQD)/module.cppm $(SQD_PCMS)
	$(CXX) $(CXXFLAGS) -w -c $< -fmodule-output=$(BUILD)/pcm/sq.data.pcm -o $(BUILD)/obj/sq.data/module.o
$(BUILD)/obj/sq.data/%.cpp.o: $(SQD)/src/%.cpp $(BUILD)/pcm/sq.data.pcm
	$(CXX) $(CXXFLAGS) -w -c $< -o $@
$(BUILD)/obj/yyjson.o: third_party/yyjson-0.12.0/src/yyjson.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pcm/sqedit.peg.pcm $(BUILD)/obj/peg.o &: src/peg.cppm $(BUILD)/pcm/std.pcm
	@mkdir -p $(BUILD)/pcm $(BUILD)/obj
	$(CXX) $(CXXFLAGS) -c $< -fmodule-output=$(BUILD)/pcm/sqedit.peg.pcm -o $(BUILD)/obj/peg.o
$(BUILD)/pcm/sqedit.grammars.pcm $(BUILD)/obj/grammars.o &: src/grammars.cppm $(wildcard grammars/*.peg) $(BUILD)/pcm/sqedit.peg.pcm
	$(CXX) $(CXXFLAGS) -c $< -fmodule-output=$(BUILD)/pcm/sqedit.grammars.pcm -o $(BUILD)/obj/grammars.o

# The library: what a tool that parses text links (sqedit.peg, and the
# grammars sqedit ships).
$(LIB): $(BUILD)/obj/peg.o $(BUILD)/obj/grammars.o
	$(AR) rcs $@ $^

$(BUILD)/sqedit: tools/sqedit.cpp $(OBJS) $(BUILD)/pcm/sq.data.pcm $(BUILD)/pcm/sqedit.grammars.pcm $(LIB)
	$(CXX) $(CXXFLAGS) $< $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/test_%: tests/test_%.cpp $(OBJS) $(BUILD)/pcm/sqedit.grammars.pcm
	$(CXX) $(CXXFLAGS) $< $(OBJS) $(LDFLAGS) -o $@

test: all
	@set -e; for t in $(TESTS); do $$t; done
	@sh tests/run_check.sh $(BUILD)/sqedit

clean:
	rm -r -f -- $(BUILD)
