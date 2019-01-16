ifeq ($(ERL_EI_INCLUDE_DIR),)
$(error ERL_EI_INCLUDE_DIR not set. Invoke via mix)
endif

SOD_RELEASE_DIR ?= c_src/sod_release_118
CFLAGS += -Wall -Wextra -fPIC -O2 -std=c99 -I$(ERL_EI_INCLUDE_DIR) -I$(SOD_RELEASE_DIR)
LDFLAGS += -fPIC -shared -L$(ERL_EI_LIBDIR) 


ifeq ($(MIX_TARGET),host)
else
CFLAGS += $(ERL_CFLAGS)
LDFLAGS += $(ERL_LDFLAGS)
endif

.DEFAULT_GOAL: all
.PHONY: all clean

all: priv priv/erl_sod_nif.so

priv:
	mkdir -p priv

$(SOD_RELEASE_DIR)/sod.o: $(SOD_RELEASE_DIR)/sod.c
	$(CC) $(CFLAGS) $(LDFLAGS) $(SOD_RELEASE_DIR)/sod.c -o $(SOD_RELEASE_DIR)/sod.o

priv/erl_sod_nif.so: c_src/enif_util.c c_src/erl_sod_nif.c c_src/queue.c $(SOD_RELEASE_DIR)/sod.o
	$(CC) $(CFLAGS) $(LDFLAGS) \
		c_src/enif_util.c c_src/erl_sod_nif.c c_src/queue.c \
		$(SOD_RELEASE_DIR)/sod.o \
		-o priv/erl_sod_nif.so

clean:
	$(RM) priv/erl_sod_nif.so
	$(RM) $(SOD_RELEASE_DIR)/sod.o