PROG1   = detectx
OBJS1   = main.c ACAP.c cJSON.c Model.c Video.c Output.c imgprovider.c imgutils.c MQTT.c CERTS.c
PROGS   = $(PROG1)
LIBDIR  = lib
INCDIR  = include

PKGS = gio-2.0 gio-unix-2.0 liblarod vdostream fcgi axevent libcurl

CFLAGS += -I./$(INCDIR) -DLAROD_API_VERSION_3
LDFLAGS += -L./$(LIBDIR) -Wl,-rpath,'$$ORIGIN/lib'
LDLIBS  += -ljpeg -lturbojpeg -s -lm -ldl -lpthread

CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))

CFLAGS += -Wall -Wno-format-overflow

all: $(PROGS)

$(PROG1): $(OBJS1)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf $(PROGS) *.o *.eap* *_LICENSE.txt package.conf* param.conf tmp* $(LIBDIR) manifest.json
