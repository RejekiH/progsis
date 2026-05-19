# ============================================================
# Makefile - Game Catur Linux
# ============================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lpthread -lrt
TARGET  = chess_thread
SRC     = chess_thread.c

.PHONY: all clean run install-deps help

all: $(TARGET)
	@echo "✓ Kompilasi berhasil: $(TARGET)"

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

# Install dependensi (Rocky Linux / RHEL)
install-deps:
	@echo "Menginstall dependensi..."
	sudo dnf install -y gcc glibc-devel 2>/dev/null || \
	sudo yum install -y gcc glibc-devel 2>/dev/null || \
	sudo apt-get install -y gcc libc-dev 2>/dev/null || \
	echo "Silakan install gcc secara manual"

# Jalankan program utama
run: all
	chmod +x chess.sh chess_engine.awk
	./chess.sh

# Jalankan thread demo saja
run-thread: all
	./$(TARGET) --interactive

# Test kompilasi
test: all
	@echo "Test kompilasi berhasil"
	./$(TARGET) &
	sleep 1
	kill -USR2 $$! 2>/dev/null || true
	sleep 1
	kill -TERM $$! 2>/dev/null || true
	@echo "Test selesai"

clean:
	rm -f $(TARGET) *.o /tmp/chess_*

help:
	@echo "=== Makefile Game Catur Linux ==="
	@echo ""
	@echo "Perintah:"
	@echo "  make              - Kompilasi program C"
	@echo "  make run          - Kompilasi + jalankan game"
	@echo "  make run-thread   - Jalankan demo thread saja"
	@echo "  make install-deps - Install gcc (butuh sudo)"
	@echo "  make clean        - Hapus file hasil kompilasi"
	@echo "  make help         - Tampilkan bantuan ini"
	@echo ""
	@echo "Kompilasi manual:"
	@echo "  gcc -Wall -o chess_thread chess_thread.c -lpthread -lrt"

# Kompilasi chess_shm (Shared Memory Array + Semaphore)
chess_shm: chess_shm.c
	$(CC) $(CFLAGS) -o chess_shm chess_shm.c $(LDFLAGS)
	@echo "✓ Kompilasi chess_shm berhasil"
