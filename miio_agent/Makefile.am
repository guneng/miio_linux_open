bin_PROGRAMS = miio_agent agent_client agent_sendline
miio_agent_SOURCES = src/miio_agent.c src/rbtree/rbtree.c src/mainloop/mainloop.c

agent_client_SOURCES = src/agent_client.c
agent_sendline_SOURCES = src/agent_sendline.c

miio_agent_LDADD = $(jsonc_LIBS) -ljson-c -lm
miio_agent_CFLAGS = -I. $(jsonc_CFLAGS) -Wall -Werror -std=gnu89

distclean-local:
	rm -rf Makefile.in compile autom4te.cache aclocal.m4 config.h.in configure depcomp install-sh missing
