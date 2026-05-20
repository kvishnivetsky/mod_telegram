include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_telegram

# ── tdlib (Telegram protocol / signaling) ────────────────────────────────────
TDLIB_DIR      = $(switch_srcdir)/libs/tdlib
TDLIB_BUILDDIR = $(switch_builddir)/libs/tdlib/build
TDLIB_LIB      = $(TDLIB_BUILDDIR)/libtd.a

# ── tg_owt (Telegram's WebRTC fork — media transport foundation) ──────────────
TG_OWT_DIR      = $(switch_srcdir)/libs/tg_owt
TG_OWT_BUILDDIR = $(switch_builddir)/libs/tg_owt/build
TG_OWT_LIB      = $(TG_OWT_BUILDDIR)/libtg_owt.a

# ── tgcalls (voice + video call media layer) ──────────────────────────────────
TGCALLS_DIR      = $(switch_srcdir)/libs/tgcalls
TGCALLS_BUILDDIR = $(switch_builddir)/libs/tgcalls/build
TGCALLS_LIB      = $(TGCALLS_BUILDDIR)/libtgcalls.a

mod_LTLIBRARIES = mod_telegram.la
mod_telegram_la_SOURCES  = mod_telegram.cpp
mod_telegram_la_CFLAGS   = $(AM_CFLAGS)
mod_telegram_la_CXXFLAGS = $(SWITCH_AM_CXXFLAGS) \
                            -I$(TDLIB_DIR) \
                            -I$(TDLIB_DIR)/td/generate/auto \
                            -I$(TGCALLS_DIR) \
                            -I$(TGCALLS_DIR)/tgcalls \
                            -I$(TG_OWT_DIR)/src \
                            -I$(TG_OWT_DIR)/src/third_party/abseil-cpp \
                            -DWEBRTC_LINUX -DWEBRTC_POSIX
mod_telegram_la_LIBADD   = $(switch_builddir)/libfreeswitch.la \
                            $(TDLIB_LIB) \
                            -lssl -lcrypto -lz -lpthread -ldl -lm
mod_telegram_la_LDFLAGS  = -avoid-version -module -no-undefined -shared \
                           -Wl,--start-group,$(TGCALLS_LIB),$(TG_OWT_LIB),--end-group \
                           -Wl,-lopus,-lavcodec,-lavutil,-lopenh264,-lvpx

BUILT_SOURCES = $(TDLIB_LIB) $(TG_OWT_LIB) $(TGCALLS_LIB)

# Build order: tg_owt first (tgcalls depends on it), then tgcalls, then tdlib.
$(TDLIB_LIB): $(TDLIB_DIR)
	$(MKDIR_P) $(TDLIB_BUILDDIR)
	cd $(TDLIB_BUILDDIR) && $(CMAKE) -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		$(switch_srcdir)/libs/tdlib
	cd $(TDLIB_BUILDDIR) && $(MAKE) tdclient
	cd $(TDLIB_BUILDDIR) && \
		find . -name "*.a" -not -name "libtd.a" | \
		awk 'BEGIN{print "CREATE libtd.a"} {print "ADDLIB " $$0} END{print "SAVE\nEND"}' | \
		ar -M

$(TG_OWT_LIB): $(TG_OWT_DIR)
	$(MKDIR_P) $(TG_OWT_BUILDDIR)
	cd $(TG_OWT_BUILDDIR) && $(CMAKE) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DBUILD_SHARED_LIBS=OFF \
		-DTG_OWT_USE_X11=OFF \
		-DTG_OWT_USE_PIPEWIRE=OFF \
		-DTG_OWT_BUILD_AUDIO_BACKENDS=OFF \
		$(switch_srcdir)/libs/tg_owt
	cd $(TG_OWT_BUILDDIR) && $(MAKE) tg_owt

$(TGCALLS_LIB): $(TGCALLS_DIR) $(TG_OWT_LIB)
	$(MKDIR_P) $(TGCALLS_BUILDDIR)
	cd $(TGCALLS_BUILDDIR) && $(CMAKE) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DTGCALLS_BUILD_GROUP_CALLS=OFF \
		-Dtg_owt_DIR=$(TG_OWT_BUILDDIR) \
		$(switch_srcdir)/libs/tgcalls
	cd $(TGCALLS_BUILDDIR) && $(MAKE) tgcalls

$(mod_telegram_la_SOURCES): $(BUILT_SOURCES)
