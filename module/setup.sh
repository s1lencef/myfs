#!/bin/bash
# setup.sh — подготовка демонстрационного окружения для MYFS.
#
# Что делает скрипт:
#   1) создаёт файл-образ заданного размера;
#   2) привязывает его к loop-устройству (/dev/loopX);
#   3) загружает модуль myfs.ko с параметрами;
#   4) монтирует ФС в /mnt/myfs.
#
# Запускать от root (sudo). Все шаги логируются.
#
# Параметры можно переопределить переменными окружения, например:
#   IMG_SIZE_MB=64 SB_COPY_OFFSET=128 M=4 sudo ./setup.sh

set -e

# --- Настройки (можно переопределить через окружение) ---
IMG_FILE="${IMG_FILE:-/tmp/myfs.img}"   # путь к файлу-образу
IMG_SIZE_MB="${IMG_SIZE_MB:-32}"        # размер образа в МБ
MOUNT_DIR="${MOUNT_DIR:-/mnt/myfs}"     # точка монтирования
MODULE="${MODULE:-./myfs.ko}"           # путь к собранному модулю

# Параметры модуля (по условию задания):
SB_OFFSET="${SB_OFFSET:-0}"             # сектор первой копии суперблока
SB_COPY_OFFSET="${SB_COPY_OFFSET:-64}"  # сектор второй копии суперблока
MAX_NAME_LEN="${MAX_NAME_LEN:-32}"      # макс. длина имени файла
M="${M:-2}"                             # размер файла в секторах (M)

echo "=== MYFS setup ==="
echo "Image:           $IMG_FILE (${IMG_SIZE_MB} MB)"
echo "Mount dir:       $MOUNT_DIR"
echo "Module:          $MODULE"
echo "sb_offset:       $SB_OFFSET"
echo "sb_copy_offset:  $SB_COPY_OFFSET"
echo "max_name_len:    $MAX_NAME_LEN"
echo "max_file_sectors (M): $M"
echo

# 1) Создаём файл-образ (заполнен нулями).
echo "[1/4] Creating image file..."
dd if=/dev/zero of="$IMG_FILE" bs=1M count="$IMG_SIZE_MB" status=none
echo "  done."

# 2) Привязываем образ к свободному loop-устройству.
echo "[2/4] Attaching loop device..."
LOOP_DEV=$(losetup --find --show "$IMG_FILE")
echo "  attached: $LOOP_DEV"

# 3) Загружаем модуль с параметрами.
echo "[3/4] Loading kernel module..."
# Выгрузим, если уже загружен (на случай повторного запуска).
if lsmod | grep -q '^myfs'; then
	umount "$MOUNT_DIR" 2>/dev/null || true
	rmmod myfs 2>/dev/null || true
fi
insmod "$MODULE" \
	device_name="$LOOP_DEV" \
	sb_offset="$SB_OFFSET" \
	sb_copy_offset="$SB_COPY_OFFSET" \
	max_name_len="$MAX_NAME_LEN" \
	max_file_sectors="$M"
echo "  module loaded. dmesg tail:"
dmesg | tail -n 5 | sed 's/^/    /'

# 4) Монтируем ФС.
echo "[4/4] Mounting filesystem..."
mkdir -p "$MOUNT_DIR"
mount -t myfs "$LOOP_DEV" "$MOUNT_DIR"
echo "  mounted at $MOUNT_DIR"
echo
echo "Files in the filesystem:"
ls -la "$MOUNT_DIR" | head -n 12
echo
echo "Loop device used: $LOOP_DEV"
echo "Save it for teardown: export LOOP_DEV=$LOOP_DEV"
echo
echo "=== setup complete ==="
echo "Now run the userspace tool, e.g.:"
echo "  ../userspace/myfs_tool $MOUNT_DIR test"
echo "  ../userspace/myfs_tool $MOUNT_DIR meta"
echo "  ../userspace/myfs_tool $MOUNT_DIR map 0"
