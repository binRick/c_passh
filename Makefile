
SUBMODULE_SRCS := \
				  submodules/chan/src/chan.c \
				  submodules/chan/src/queue.c \
				  submodules/c_stringfn/src/stringfn.c \
				  submodules/c_string_buffer/src/stringbuffer.c \
				  submodules/c_timer/src/c_timer.c \
				  submodules/debug-memory/debug_memory.c \
				  submodules/timestamp/timestamp.c
INCLUDE_PATHS = \
				  -Isubmodules/debug-memory \
				  -Isubmodules/chan/src \
				  -Isubmodules/c_stringfn/include \
				  -Isubmodules/c_stringfn/src \
				  -Isubmodules/c_string_buffer/include \
				  -Isubmodules/c_string_buffer/src \
				  -Isubmodules/c_timer/src \
				  -Isubmodules/c_timer/include 

C_ARGS = -Wno-pragma-once-outside-header

all: passh


passh: 
	@gcc $(C_ARGS) $(SUBMODULE_SRCS) $(INCLUDE_PATHS) passh.c -o passh
clean:
	-rm passh

.PHONY: all clean
