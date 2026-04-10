PREFIX  ?= $(HOME)/.local
BIN      = iss
LABEL    = com.instant-swipe
AGENT_DIR = $(HOME)/Library/LaunchAgents
PLIST     = $(AGENT_DIR)/$(LABEL).plist
MODE     ?=

CFLAGS  ?= -std=c11 -O3 -march=native -pipe -flto -Wall -Wextra -Wpedantic
LDFLAGS  = -framework ApplicationServices -framework CoreFoundation \
           -Wl,-dead_strip -Wl,-dead_strip_dylibs -Wl,-x

$(BIN): iss.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

lint: iss.c
	cppcheck --enable=warning,style,performance --error-exitcode=1 iss.c

install: $(BIN)
	@mode='$(MODE)'; \
	if [ -z "$$mode" ]; then \
	  if [ -t 0 ]; then \
	    printf 'Install mode [both/trackpad/shortcuts] (default: both): '; \
	    read -r mode; \
	  fi; \
	  [ -n "$$mode" ] || mode=both; \
	fi; \
	case "$$mode" in \
	  both|trackpad|shortcuts) ;; \
	  *) printf 'Invalid MODE: %s\n' "$$mode" >&2; exit 1 ;; \
	esac; \
	install -d $(PREFIX)/bin; \
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN); \
	codesign -fs - $(PREFIX)/bin/$(BIN); \
	xattr -d com.apple.quarantine $(PREFIX)/bin/$(BIN) 2>/dev/null || true; \
	mkdir -p $(AGENT_DIR); \
	{ \
	  printf '%s\n' \
	    '<?xml version="1.0" encoding="UTF-8"?>' \
	    '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"' \
	    '  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	    '<plist version="1.0">' \
	    '<dict>' \
	    '  <key>Label</key>' \
	    '  <string>$(LABEL)</string>' \
	    '  <key>ProgramArguments</key>' \
	    '  <array>' \
	    '    <string>$(PREFIX)/bin/$(BIN)</string>' \
	    "    <string>--mode=$$mode</string>" \
	    '  </array>' \
	    '  <key>RunAtLoad</key>' \
	    '  <true/>' \
	    '  <key>KeepAlive</key>' \
	    '  <true/>' \
	    '</dict>' \
	    '</plist>'; \
	} > $(PLIST); \
	launchctl bootout gui/$$(id -u) $(PLIST) 2>/dev/null || true; \
	launchctl bootstrap gui/$$(id -u) $(PLIST); \
	printf 'Installed %s in %s mode\n' '$(BIN)' "$$mode"

uninstall:
	launchctl bootout gui/$$(id -u) $(PLIST) 2>/dev/null || true
	rm -f $(PREFIX)/bin/$(BIN) $(PLIST)

clean:
	rm -f $(BIN)

.PHONY: install uninstall clean
