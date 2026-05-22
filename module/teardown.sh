#!/bin/bash
# teardown.sh — снос демонстрационного окружения MYFS.
#
# Размонтирует ФС, выгружает модуль и отвязывает loop-устройство.
# Запускать от root (sudo).
#
# Если LOOP_DEV не задан, скрипт попробует найти loop-устройство по образу.

MOUNT_DIR="${MOUNT_DIR:-/mnt/myfs}"
IMG_FILE="${IMG_FILE:-/tmp/myfs.img}"

echo "=== MYFS teardown ==="

# 1) Размонтируем (если смонтировано).
if mountpoint -q "$MOUNT_DIR"; then
	echo "[1/3] Unmounting $MOUNT_DIR..."
	umount "$MOUNT_DIR"
else
	echo "[1/3] $MOUNT_DIR not mounted, skipping."
fi

# 2) Выгрузим модуль (если загружен).
if lsmod | grep -q '^myfs'; then
	echo "[2/3] Removing module..."
	rmmod myfs
else
	echo "[2/3] module not loaded, skipping."
fi

# 3) Отвяжем loop-устройство.
echo "[3/3] Detaching loop device..."
if [ -n "$LOOP_DEV" ]; then
	losetup -d "$LOOP_DEV" && echo "  detached $LOOP_DEV"
else
	# Найдём по образу.
	DEV=$(losetup -j "$IMG_FILE" | cut -d: -f1)
	if [ -n "$DEV" ]; then
		losetup -d "$DEV" && echo "  detached $DEV"
	else
		echo "  no loop device found for $IMG_FILE"
	fi
fi

echo "=== teardown complete ==="
