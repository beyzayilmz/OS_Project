# Derleyici
CC = gcc

# Derleme Bayrakları
# -Wall: Uyarıları aç
# -pthread: Thread desteği (macOS'ta da gereklidir/zararı yoktur)
CFLAGS = -Wall -pthread

# Bağlayıcı Bayrakları (Linker Flags)
# macOS'ta -lrt GEREKMEZ, bu yüzden burayı boş bırakıyoruz.
LDFLAGS = 

# Hedef dosya adı
TARGET = procx

# Kaynak dosyası
SRC = procx.c

# Varsayılan kural
all: $(TARGET)

# Derleme kuralı
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

# Temizlik kuralı
clean:
	rm -f $(TARGET)