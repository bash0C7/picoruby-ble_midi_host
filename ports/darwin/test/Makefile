MIDI_DIR ?= ../../../../picoruby-midi

CC     ?= cc
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 -I$(MIDI_DIR)/include -I$(MIDI_DIR)/ports/darwin/include

SRC = ../ble_midi_host.c test_ble_midi_host_darwin.c \
      $(MIDI_DIR)/src/midi_transport_registry.c $(MIDI_DIR)/src/midi_route.c $(MIDI_DIR)/src/midi_parser.c \
      $(MIDI_DIR)/src/midi_scheduler.c $(MIDI_DIR)/src/midi_clock_gen.c $(MIDI_DIR)/src/ble_midi_codec.c \
      $(MIDI_DIR)/ports/darwin/midi.c $(MIDI_DIR)/ports/darwin/midi_timed_ring.c
BIN = test_ble_midi_host_darwin

run: $(BIN)
	./$(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -f $(BIN)

.PHONY: run clean
