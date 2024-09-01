PROG1	= detectx
OBJS1	= main.c ACAP.c cJSON.c Model.c Video.c imgprovider.c
PROGS	= $(PROG1)

PKGS = gio-2.0 gio-unix-2.0 liblarod vdostream axhttp

LDFLAGS += -L./$(LIBDIR) -Wl,-rpath,'$$ORIGIN/$(LIBDIR)'

CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))
LDLIBS  += -s -lm -ldl -lyuv -laxparameter

CFLAGS += -Wall -DLAROD_API_VERSION_3


all:	$(PROGS)

$(PROG1): $(OBJS1)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf $(PROGS) *.o $(LIBDIR) *.eap* *_LICENSE.txt manifest.json package.conf* param.conf
