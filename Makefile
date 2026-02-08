PLUGIN_NAME = MultiRolandDriver
BUNDLE      = $(PLUGIN_NAME).plugin

CXX      = clang++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -arch arm64 -arch x86_64 \
           -mmacosx-version-min=12.0 -fvisibility=hidden
LDFLAGS  = -bundle -arch arm64 -arch x86_64 -mmacosx-version-min=12.0
FRAMEWORKS = -framework CoreMIDI -framework CoreFoundation -framework IOKit

SOURCES = Sources/MultiRolandDriver.cpp \
          Sources/RolandUSBDevice.cpp \
          Sources/USBMIDIParser.cpp

OBJECTS = $(SOURCES:.cpp=.o)

all: $(BUNDLE)

INSTALL_DIR = $(HOME)/Library/Audio/MIDI Drivers

$(BUNDLE): $(OBJECTS) Resources/Info.plist
	@mkdir -p $(BUNDLE)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(FRAMEWORKS) $(OBJECTS) -o $(BUNDLE)/Contents/MacOS/$(PLUGIN_NAME)
	@cp Resources/Info.plist $(BUNDLE)/Contents/
	codesign --force --sign - $(BUNDLE)
	@echo "Built and signed $(BUNDLE)"

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

install: $(BUNDLE)
	@mkdir -p "$(INSTALL_DIR)"
	cp -R $(BUNDLE) "$(INSTALL_DIR)/"
	killall MIDIServer 2>/dev/null || true
	@echo "Installed to ~/Library/Audio/MIDI Drivers/"

uninstall:
	rm -rf "$(INSTALL_DIR)/$(BUNDLE)"
	killall MIDIServer 2>/dev/null || true
	@echo "Uninstalled from ~/Library/Audio/MIDI Drivers/"

clean:
	rm -rf $(BUNDLE) $(OBJECTS)

.PHONY: all install uninstall clean
