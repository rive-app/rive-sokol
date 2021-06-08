
ifndef config
  config=debug
endif

ifeq ($(origin CC), default)
  CC = clang
endif
ifeq ($(origin CXX), default)
  CXX = clang++
endif
ifeq ($(origin AR), default)
  AR = ar
endif

BUILDNAME_BINARY  = rive_sokol
BUILDNAME_LIBRARY = librivesokol.a
BIULDDIR          = build
DEPENDDIR         = $(BIULDDIR)/dependencies
TARGETDIR         = $(BIULDDIR)/bin/$(config)
TARGET            = $(TARGETDIR)/$(BUILDNAME_BINARY)
LIBRARY           = $(TARGETDIR)/$(BUILDNAME_LIBRARY)
OBJDIR            = $(BIULDDIR)/obj/$(config)

INCLUDES       += -Isrc -I$(DEPENDDIR)/glfw/include -I$(DEPENDDIR)/sokol -I$(DEPENDDIR)/rive-cpp/include -I$(DEPENDDIR)/jc_containers/src -I$(DEPENDDIR)/libtess2/Include -I$(DEPENDDIR)/linmath.h -I$(DEPENDDIR)/imgui
ALL_CPPFLAGS   += $(CPPFLAGS) -MMD -MP $(DEFINES) $(INCLUDES)
ALL_LDFLAGS    += $(LDFLAGS) $(PLATFORM_LDFLAGS) -L$(TARGETDIR) -L$(DEPENDDIR)/glfw_build/src -L$(DEPENDDIR)/rive-cpp/build/bin/${config} -L$(DEPENDDIR)/libtess2/Build
LIBS 		   += -lglfw3 -lrive -lrivesokol -ltess2_${config} $(PLATFORM_LIBS)
LINKCMD         = $(CXX) -o "$@" $(OBJECTS) $(ALL_LDFLAGS) $(LIBS)
LINKCMD_LIBRARY = $(AR) -rcs "$@" $(OBJECTS_LIBRARY)

ifeq ($(config),debug)
	DEFINES      += -DDEBUG
	ALL_CFLAGS   += $(CFLAGS) $(ALL_CPPFLAGS) -g -Wall -fno-exceptions -fno-rtti
	ALL_CXXFLAGS += $(CXXFLAGS) $(ALL_CPPFLAGS) -g -Wall -std=c++17 -fno-exceptions -fno-rtti
else ifeq ($(config),release)
	DEFINES      += -DRELEASE -DNDEBUG
	ALL_CFLAGS   += $(CFLAGS) $(ALL_CPPFLAGS) -O2 -Wall -fno-exceptions -fno-rtti
	ALL_CXXFLAGS += $(CXXFLAGS) $(ALL_CPPFLAGS) -O2 -Wall -std=c++17 -fno-exceptions -fno-rtti
endif

OBJECTS_LIBRARY := \
	$(OBJDIR)/rive_render_private.o \
	$(OBJDIR)/rive_render_tss.o \
	$(OBJDIR)/rive_render_stc.o \

OBJECTS := \
	$(OBJDIR)/main.o \
	$(OBJDIR)/app.o \
	$(OBJDIR)/imgui.o \
	$(OBJDIR)/imgui_draw.o \
	$(OBJDIR)/imgui_widgets.o \
	$(OBJDIR)/imgui_tables.o \

.PHONY: clean prebuild

all: prebuild $(LIBRARY) $(TARGET)

library: prebuild $(LIBRARY)

viewer: prebuild $(TARGET)

$(LIBRARY): $(OBJECTS_LIBRARY) | $(TARGETDIR)
	@echo Linking library
	$(LINKCMD_LIBRARY)

$(TARGET): $(OBJECTS) | $(TARGETDIR)
	@echo Linking
	$(LINKCMD)

$(TARGETDIR):
	@echo Creating $(TARGETDIR)
	mkdir -p $(TARGETDIR)

$(OBJDIR):
	@echo Creating $(OBJDIR)
	mkdir -p $(OBJDIR)

prebuild: | $(OBJDIR)

clean:
	rm -f $(TARGET)
	rm -f $(LIBRARY)
	rm -rf $(OBJDIR)

$(OBJDIR)/main.o: src/main.cpp
	@echo $(notdir $<)
	$(CXX) $(ALL_CXXFLAGS) -o "$@" -MF "$(@:%.o=%.d)" -c "$<"
$(OBJDIR)/app.o: src/app.cpp
	@echo $(notdir $<)
	$(CXX) $(ALL_CXXFLAGS) -o "$@" -MF "$(@:%.o=%.d)" -c "$<"
$(OBJDIR)/rive_render_private.o: src/rive/rive_render_private.cpp
	@echo $(notdir $<)
	$(CXX) $(ALL_CXXFLAGS) -o "$@" -MF "$(@:%.o=%.d)" -c "$<"
$(OBJDIR)/rive_render_tss.o: src/rive/rive_render_private_tss.cpp
	@echo $(notdir $<)
	$(CXX) $(ALL_CXXFLAGS) -o "$@" -MF "$(@:%.o=%.d)" -c "$<"
$(OBJDIR)/rive_render_stc.o: src/rive/rive_render_private_stc.cpp
	@echo $(notdir $<)
	$(CXX) $(ALL_CXXFLAGS) -o "$@" -MF "$(@:%.o=%.d)" -c "$<"
$(OBJDIR)/imgui.o: $(DEPENDDIR)/imgui/imgui.cpp
	@echo $(notdir $<)
	$(CXX) $(ALL_CXXFLAGS) -o "$@" -MF "$(@:%.o=%.d)" -c "$<"
$(OBJDIR)/imgui_draw.o: $(DEPENDDIR)/imgui/imgui_draw.cpp
	@echo $(notdir $<)
	$(CXX) $(ALL_CXXFLAGS) -o "$@" -MF "$(@:%.o=%.d)" -c "$<"
$(OBJDIR)/imgui_widgets.o: $(DEPENDDIR)/imgui/imgui_widgets.cpp
	@echo $(notdir $<)
	$(CXX) $(ALL_CXXFLAGS) -o "$@" -MF "$(@:%.o=%.d)" -c "$<"
$(OBJDIR)/imgui_tables.o: $(DEPENDDIR)/imgui/imgui_tables.cpp
	@echo $(notdir $<)
	$(CXX) $(ALL_CXXFLAGS) -o "$@" -MF "$(@:%.o=%.d)" -c "$<"
