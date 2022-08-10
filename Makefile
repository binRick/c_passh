
SUBMODULE_SRCS := \
				  submodules/c_deps/submodules/chan/src/chan.c \
				  submodules/c_deps/submodules/chan/src/queue.c \
				  submodules/c_deps/submodules/c_stringfn/src/stringfn.c \
				  submodules/c_deps/submodules/c_string_buffer/src/stringbuffer.c \
				  submodules/c_deps/submodules/c_vector/src/vector.c \
				  submodules/c_deps/submodules/c_timer/src/c_timer.c \
				  submodules/c_deps/submodules/debug-memory/debug_memory.c \
				  submodules/c_deps/submodules/timestamp/timestamp.c
INCLUDE_PATHS = \
				  -Isubmodules/c_deps/submodules/debug-memory \
				  -Isubmodules/c_deps/submodules/chan/src \
				  -Isubmodules/c_deps/submodules/c_stringfn/include \
				  -Isubmodules/c_deps/submodules/c_stringfn/src \
				  -Isubmodules/c_deps/submodules/c_string_buffer/include \
				  -Isubmodules/c_deps/submodules/c_string_buffer/src \
				  -Isubmodules/c_deps/submodules/c_vector/include \
				  -Isubmodules/c_deps/submodules/c_vector/src \
				  -Isubmodules/c_deps/submodules/c_timer/src \
				  -Isubmodules/c_deps/submodules/c_timer/include 
C_ARGS = -Wno-pragma-once-outside-header

all: passh


passh: 
	@gcc $(C_ARGS) $(SUBMODULE_SRCS) $(INCLUDE_PATHS) passh.c -o passh
clean:
	-rm passh

.PHONY: all clean
