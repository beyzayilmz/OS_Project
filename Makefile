CC = gcc
CFLAGS = -Wall -g -pthread
LDFLAGS = -lrt -pthread

TARGET = procx
SRC = procx.c
OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
	# IPC nesnelerini temizlemek için manuel komutlar gerekebilir
	# örn: rm /dev/shm/procx_shm