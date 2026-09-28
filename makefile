# Detect Operating System
UNAME_S := $(shell uname -s)

INCLUDE = -I../hdr

# Cross-platform Homebrew / Local path resolution
ifeq ($(UNAME_S), Darwin)
    # macOS: Check Apple Silicon path first, then Intel
    HOMEBREW_PREFIX = $(shell if [ -d /opt/homebrew ]; then echo "/opt/homebrew"; else echo "/usr/local"; fi)
    SYS_INCLUDE = -I$(HOMEBREW_PREFIX)/include
    SYS_LIB = -L$(HOMEBREW_PREFIX)/lib
else ifeq ($(OS), Windows_NT)
    # Windows (MinGW/Cygwin)
    SYS_INCLUDE = 
    SYS_LIB = 
else
    # Linux
    SYS_INCLUDE = -I/usr/include
    SYS_LIB = -L/usr/lib -L/usr/local/lib
endif

# Compilation Flags
CFLAGS  = $(INCLUDE) $(SYS_INCLUDE) -I$(COMM_HDR_INC) -I$(EXT_HDR_INC)
CC      = gcc -g -fPIC
CC1     = gcc

SRC = logger.c
OBJ = $(SRC:.c=.o)
_DEPS = hssLog_defs.h

all: libhsszlog.so

# Pattern rule for compiling source files to object files
%.o: %.c $(_DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

hsszlog_log: $(TESTOBJ)
	$(CC1) -L. -o $@ $(TESTOBJ) -lhsszlog

# Fixed linking line: merged custom DB libs and system libs cleanly
libhsszlog.so: $(OBJ)
	$(CC) -shared -o $@ $(OBJ) $(RTLIBLINK) $(THREADLIB) $(SYS_LIB) $(if $(HSS_DB_LIB),-L$(HSS_DB_LIB)) -lzlog

clean: libhsszlogclean

libhsszlogclean:
	rm -f *.o libhsszlog.so

