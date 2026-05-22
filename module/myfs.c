// SPDX-License-Identifier: GPL-2.0
/*
 * myfs.c — файловая система на базе блочного устройства.
 *
 * Возможности:
 *   - Две копии суперблока (в начале диска и со смещением sb_copy_offset).
 *   - Контроль целостности суперблока через контрольную сумму (CRC32).
 *   - Все файлы создаются при инициализации ФС, имеют одинаковый размер
 *     (M секторов), один файл = один участок диска.
 *   - Регистрация в VFS: ФС монтируется (в /mnt), файлы видны в userspace,
 *     поддерживаются чтение и запись.
 *   - Набор ioctl: обнуление всех файлов, стирание ФС, получение
 *     метаинформации (хеши), получение маппинга секторов файла.
 *
 * Важная архитектурная деталь.
 * Для блочной ФС каноничный путь монтирования — mount_bdev(): ядро само
 * открывает блочное устройство (его имя берётся из команды mount) и настраивает
 * инфраструктуру buffer cache, после чего вызывает наш fill_super. По условию
 * задания имя диска также задаётся ПАРАМЕТРОМ МОДУЛЯ device_name, а смещения
 * суперблоков и геометрия — тоже параметрами модуля; в fill_super мы используем
 * именно эти параметры. Если device_name задан, при монтировании выполняется
 * сверка с устройством из команды mount (предупреждение в лог при расхождении).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/uaccess.h>
#include <linux/pagemap.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/vmalloc.h>

#include "myfs.h"

/* ===========================================================================
 *  Параметры модуля
 * ======================================================================== */

/* Имя блочного устройства, на котором живёт ФС, например "/dev/loop0". */
static char *device_name = "";
module_param(device_name, charp, 0444);
MODULE_PARM_DESC(device_name, "Path to the backing block device, e.g. /dev/loop0");

/* Смещение (в секторах) для ПЕРВОЙ копии суперблока. По умолчанию 0. */
static unsigned int sb_offset = 0;
module_param(sb_offset, uint, 0444);
MODULE_PARM_DESC(sb_offset, "Sector offset of the primary superblock (default 0)");

/* Смещение (в секторах) для ВТОРОЙ (резервной) копии суперблока. */
static unsigned int sb_copy_offset = 64;
module_param(sb_copy_offset, uint, 0444);
MODULE_PARM_DESC(sb_copy_offset, "Sector offset of the backup superblock copy");

/* Максимальная длина имени файла. */
static unsigned int max_name_len = 32;
module_param(max_name_len, uint, 0444);
MODULE_PARM_DESC(max_name_len, "Maximum file name length");

/* Максимальный размер файла в секторах (M). */
static unsigned int max_file_sectors = 1;
module_param(max_file_sectors, uint, 0444);
MODULE_PARM_DESC(max_file_sectors, "File size in sectors (M)");

/* ===========================================================================
 *  Внутренние структуры в памяти
 * ======================================================================== */

#define MYFS_FS_NAME    "myfs"
#define MYFS_ROOT_INO   1            /* номер inode корневого каталога */
#define MYFS_FIRST_INO  2            /* с какого номера начинаются файлы */

/*
 * Информация о смонтированной ФС, привязанная к struct super_block через
 * поле s_fs_info. Здесь храним открытое блочное устройство и копию
 * геометрии из суперблока.
 */
struct myfs_sb_info {
	/* Геометрия (из суперблока, продублирована для удобства): */
	u64 total_sectors;
	u64 sb_copy_offset;
	u32 max_name_len;
	u32 file_size_sectors;
	u64 first_data_sector;
	u64 file_count;

	struct mutex io_lock;       /* сериализация операций ввода-вывода */
};

/*
 * Приватные данные inode'а файла: индекс файла в ФС. По индексу однозначно
 * вычисляется положение файла на диске.
 */
struct myfs_inode_info {
	u64 file_index;             /* 0..file_count-1 */
	struct inode vfs_inode;     /* встроенный VFS-inode */
};

static inline struct myfs_inode_info *MYFS_I(struct inode *inode)
{
	return container_of(inode, struct myfs_inode_info, vfs_inode);
}

/* Кеш для аллокации наших inode'ов. */
static struct kmem_cache *myfs_inode_cachep;

/* ===========================================================================
 *  Утилиты: подсчёт контрольной суммы и доступ к блочному устройству
 * ======================================================================== */

/*
 * Контрольная сумма суперблока. Считаем CRC32 по всей структуре, но с
 * обнулённым полем checksum (иначе сумма зависела бы сама от себя).
 */
static u32 myfs_sb_checksum(const struct myfs_super_block *sb)
{
	struct myfs_super_block tmp = *sb;

	tmp.checksum = 0;
	return crc32(0, (const u8 *)&tmp, sizeof(tmp));
}

/* Контрольная сумма полезных данных файла.
 * При len==0 crc32 вернёт начальное значение (0), указатель не разыменуется,
 * но для надёжности на len==0 передаём непустой адрес. */
static u32 myfs_data_checksum(const void *data, u32 len)
{
	static const u8 empty;

	if (len == 0)
		return crc32(0, &empty, 0);
	return crc32(0, data, len);
}

/*
 * Чтение одного сектора (512 байт) с устройства в буфер buf.
 * Используем buffer_head поверх block_device — простой и надёжный способ
 * для блочного доступа из ФС.
 */
static int myfs_read_sector(struct super_block *sb, u64 sector, void *buf)
{
	struct buffer_head *bh;

	bh = sb_bread(sb, sector);
	if (!bh)
		return -EIO;

	memcpy(buf, bh->b_data, MYFS_SECTOR_SIZE);
	brelse(bh);
	return 0;
}

/* Запись одного сектора (512 байт) из буфера buf на устройство. */
static int myfs_write_sector(struct super_block *sb, u64 sector, const void *buf)
{
	struct buffer_head *bh;

	bh = sb_bread(sb, sector);
	if (!bh)
		return -EIO;

	memcpy(bh->b_data, buf, MYFS_SECTOR_SIZE);
	mark_buffer_dirty(bh);
	/* sync_dirty_buffer гарантирует, что данные ушли на устройство —
	 * для учебной ФС важна предсказуемость, а не максимальная скорость. */
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

/*
 * Чтение/запись произвольного диапазона байт внутри одного файла.
 * Файл может занимать несколько секторов, поэтому работаем посекторно.
 *
 * file_first_sector — первый сектор файла на устройстве (включая заголовок).
 * Здесь же удобно реализовать чтение/запись «сырых» секторов файла.
 */

/* Номер первого сектора файла с заданным индексом. */
static inline u64 myfs_file_first_sector(struct myfs_sb_info *si, u64 index)
{
	return si->first_data_sector + index * (u64)si->file_size_sectors;
}

/* ===========================================================================
 *  Работа с заголовком файла и его данными
 * ======================================================================== */

/* Прочитать заголовок файла (он лежит в начале первого сектора файла). */
static int myfs_read_file_header(struct super_block *sb, u64 index,
				 struct myfs_file_header *hdr)
{
	struct myfs_sb_info *si = sb->s_fs_info;
	u8 *sector;
	u64 first;
	int ret;

	sector = kmalloc(MYFS_SECTOR_SIZE, GFP_KERNEL);
	if (!sector)
		return -ENOMEM;

	first = myfs_file_first_sector(si, index);
	ret = myfs_read_sector(sb, first, sector);
	if (ret) {
		kfree(sector);
		return ret;
	}

	memcpy(hdr, sector, sizeof(*hdr));
	kfree(sector);
	return 0;
}

/* Записать заголовок файла обратно (сохраняя остальные данные в секторе). */
static int myfs_write_file_header(struct super_block *sb, u64 index,
				  const struct myfs_file_header *hdr)
{
	struct myfs_sb_info *si = sb->s_fs_info;
	u8 *sector;
	u64 first;
	int ret;

	sector = kmalloc(MYFS_SECTOR_SIZE, GFP_KERNEL);
	if (!sector)
		return -ENOMEM;

	first = myfs_file_first_sector(si, index);
	ret = myfs_read_sector(sb, first, sector);   /* читаем, чтобы не потерять данные */
	if (ret) {
		kfree(sector);
		return ret;
	}

	memcpy(sector, hdr, sizeof(*hdr));
	ret = myfs_write_sector(sb, first, sector);
	kfree(sector);
	return ret;
}

/*
 * Чтение полезных данных файла в буфер. Полезные данные начинаются сразу
 * после заголовка и могут занимать несколько секторов.
 *
 * Возвращает количество скопированных байт или отрицательный код ошибки.
 */
static int myfs_read_file_data(struct super_block *sb, u64 index,
			       loff_t pos, void *out, size_t len)
{
	struct myfs_sb_info *si = sb->s_fs_info;
	struct myfs_file_header hdr;
	u8 *filebuf;
	size_t filebytes = (size_t)si->file_size_sectors * MYFS_SECTOR_SIZE;
	u64 first = myfs_file_first_sector(si, index);
	u32 i;
	int ret;

	ret = myfs_read_file_header(sb, index, &hdr);
	if (ret)
		return ret;

	/* Если читаем за пределами реальных данных — отдаём 0 байт (EOF). */
	if (pos >= hdr.data_len)
		return 0;
	if (pos + len > hdr.data_len)
		len = hdr.data_len - pos;
	if (len == 0)
		return 0;

	filebuf = vmalloc(filebytes);
	if (!filebuf)
		return -ENOMEM;

	/* Считываем все секторы файла подряд. */
	for (i = 0; i < si->file_size_sectors; i++) {
		ret = myfs_read_sector(sb, first + i,
				       filebuf + (size_t)i * MYFS_SECTOR_SIZE);
		if (ret) {
			vfree(filebuf);
			return ret;
		}
	}

	/* Полезные данные идут после заголовка. */
	memcpy(out, filebuf + sizeof(struct myfs_file_header) + pos, len);
	vfree(filebuf);
	return (int)len;
}

/*
 * Запись полезных данных файла. Аналогично чтению: подгружаем все секторы
 * файла, модифицируем нужный участок, пересчитываем длину и контрольную
 * сумму данных в заголовке, пишем всё обратно.
 *
 * Возвращает количество записанных байт или отрицательный код ошибки.
 */
static int myfs_write_file_data(struct super_block *sb, u64 index,
				loff_t pos, const void *in, size_t len)
{
	struct myfs_sb_info *si = sb->s_fs_info;
	struct myfs_file_header hdr;
	u8 *filebuf;
	size_t filebytes = (size_t)si->file_size_sectors * MYFS_SECTOR_SIZE;
	size_t payload_cap = MYFS_FILE_PAYLOAD(si->file_size_sectors);
	u64 first = myfs_file_first_sector(si, index);
	u32 new_len;
	u32 i;
	int ret;

	/* Ограничиваем запись ёмкостью файла. */
	if (pos >= payload_cap)
		return -ENOSPC;
	if (pos + len > payload_cap)
		len = payload_cap - pos;
	if (len == 0)
		return 0;

	ret = myfs_read_file_header(sb, index, &hdr);
	if (ret)
		return ret;

	filebuf = vmalloc(filebytes);
	if (!filebuf)
		return -ENOMEM;

	/* Считываем текущее содержимое файла (нужно, чтобы не затереть
	 * данные вне модифицируемого диапазона). */
	for (i = 0; i < si->file_size_sectors; i++) {
		ret = myfs_read_sector(sb, first + i,
				       filebuf + (size_t)i * MYFS_SECTOR_SIZE);
		if (ret) {
			vfree(filebuf);
			return ret;
		}
	}

	/* Накладываем новые данные поверх. */
	memcpy(filebuf + sizeof(struct myfs_file_header) + pos, in, len);

	/* Новая длина данных = максимум из старой и (pos+len). */
	new_len = hdr.data_len;
	if (pos + len > new_len)
		new_len = pos + len;

	/* Пересчитываем заголовок: длину и хеш полезных данных. */
	hdr.magic = MYFS_MAGIC;
	hdr.data_len = new_len;
	hdr.data_checksum = myfs_data_checksum(
		filebuf + sizeof(struct myfs_file_header), new_len);
	memcpy(filebuf, &hdr, sizeof(hdr));

	/* Пишем все секторы файла обратно. */
	for (i = 0; i < si->file_size_sectors; i++) {
		ret = myfs_write_sector(sb, first + i,
					filebuf + (size_t)i * MYFS_SECTOR_SIZE);
		if (ret) {
			vfree(filebuf);
			return ret;
		}
	}

	vfree(filebuf);
	return (int)len;
}

/* ===========================================================================
 *  Файловые операции (чтение/запись/ioctl)
 * ======================================================================== */

/* read(): отдаём данные файла в userspace-буфер. */
static ssize_t myfs_read(struct file *filp, char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct inode *inode = file_inode(filp);
	struct super_block *sb = inode->i_sb;
	struct myfs_sb_info *si = sb->s_fs_info;
	struct myfs_inode_info *mi = MYFS_I(inode);
	void *kbuf;
	int ret;

	if (count == 0)
		return 0;
	/* Ограничим единичное чтение ёмкостью файла, чтобы vmalloc не был
	 * чрезмерным при огромном count. */
	if (count > MYFS_FILE_PAYLOAD(si->file_size_sectors))
		count = MYFS_FILE_PAYLOAD(si->file_size_sectors);

	kbuf = vmalloc(count);
	if (!kbuf)
		return -ENOMEM;

	mutex_lock(&si->io_lock);
	ret = myfs_read_file_data(sb, mi->file_index, *ppos, kbuf, count);
	mutex_unlock(&si->io_lock);

	if (ret <= 0) {
		vfree(kbuf);
		return ret;  /* 0 => EOF, либо отрицательная ошибка */
	}

	if (copy_to_user(buf, kbuf, ret)) {
		vfree(kbuf);
		return -EFAULT;
	}

	*ppos += ret;
	vfree(kbuf);
	return ret;
}

/* write(): принимаем данные из userspace и пишем в файл. */
static ssize_t myfs_write(struct file *filp, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct inode *inode = file_inode(filp);
	struct super_block *sb = inode->i_sb;
	struct myfs_sb_info *si = sb->s_fs_info;
	struct myfs_inode_info *mi = MYFS_I(inode);
	void *kbuf;
	int ret;

	if (count == 0)
		return 0;
	/* Ограничим единичную запись ёмкостью файла. */
	if (count > MYFS_FILE_PAYLOAD(si->file_size_sectors))
		count = MYFS_FILE_PAYLOAD(si->file_size_sectors);

	kbuf = vmalloc(count);
	if (!kbuf)
		return -ENOMEM;

	if (copy_from_user(kbuf, buf, count)) {
		vfree(kbuf);
		return -EFAULT;
	}

	mutex_lock(&si->io_lock);
	ret = myfs_write_file_data(sb, mi->file_index, *ppos, kbuf, count);
	mutex_unlock(&si->io_lock);

	if (ret < 0) {
		vfree(kbuf);
		return ret;
	}

	*ppos += ret;
	/* Обновим размер inode'а в VFS, чтобы ls показывал актуальную длину. */
	if (*ppos > i_size_read(inode))
		i_size_write(inode, *ppos);

	vfree(kbuf);
	return ret;
}

/* --- Реализация ioctl --- */

/* MYFS_IOC_ZERO_ALL: обнулить полезные данные всех файлов. */
static long myfs_ioc_zero_all(struct super_block *sb)
{
	struct myfs_sb_info *si = sb->s_fs_info;
	struct myfs_file_header hdr;
	u64 idx;
	int ret;

	for (idx = 0; idx < si->file_count; idx++) {
		/* Перечитаем заголовок, чтобы сохранить magic, и обнулим длину. */
		ret = myfs_read_file_header(sb, idx, &hdr);
		if (ret)
			return ret;

		hdr.magic = MYFS_MAGIC;
		hdr.data_len = 0;
		/* Хеш пустых данных. */
		hdr.data_checksum = myfs_data_checksum(NULL, 0);

		ret = myfs_write_file_header(sb, idx, &hdr);
		if (ret)
			return ret;
	}
	return 0;
}

/* MYFS_IOC_ERASE_FS: затереть оба суперблока (ФС станет неузнаваемой). */
static long myfs_ioc_erase_fs(struct super_block *sb)
{
	struct myfs_sb_info *si = sb->s_fs_info;
	u8 *zero;
	int ret;

	zero = kzalloc(MYFS_SECTOR_SIZE, GFP_KERNEL);
	if (!zero)
		return -ENOMEM;

	/* Первая копия (нулевой сектор по нашему соглашению — sb_offset 0). */
	ret = myfs_write_sector(sb, sb_offset, zero);
	if (ret)
		goto out;

	/* Вторая копия. */
	ret = myfs_write_sector(sb, si->sb_copy_offset, zero);
out:
	kfree(zero);
	return ret;
}

/* MYFS_IOC_GET_META: вернуть метаинформацию (хеши) по всем файлам. */
static long myfs_ioc_get_meta(struct super_block *sb, unsigned long arg)
{
	struct myfs_sb_info *si = sb->s_fs_info;
	struct myfs_meta_list req;
	struct myfs_meta_entry entry;
	struct myfs_file_header hdr;
	void __user *uentries;
	u64 idx, n;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	uentries = (void __user *)(unsigned long)req.entries_ptr;

	/* Сколько элементов реально вернём: min(file_count, capacity). */
	n = si->file_count;
	if (n > req.capacity)
		n = req.capacity;

	for (idx = 0; idx < n; idx++) {
		ret = myfs_read_file_header(sb, idx, &hdr);
		if (ret)
			return ret;

		entry.file_index = idx;
		entry.data_len = hdr.data_len;
		entry.data_checksum = hdr.data_checksum;

		if (copy_to_user(uentries + idx * sizeof(entry),
				 &entry, sizeof(entry)))
			return -EFAULT;
	}

	req.count = n;
	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

/* MYFS_IOC_GET_MAP: вернуть диапазон секторов заданного файла. */
static long myfs_ioc_get_map(struct super_block *sb, unsigned long arg)
{
	struct myfs_sb_info *si = sb->s_fs_info;
	struct myfs_sector_map req;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.file_index >= si->file_count)
		return -EINVAL;

	req.first_sector = myfs_file_first_sector(si, req.file_index);
	req.sector_count = si->file_size_sectors;

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

/* Диспетчер ioctl. */
static long myfs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct super_block *sb = inode->i_sb;
	struct myfs_sb_info *si = sb->s_fs_info;
	long ret;

	mutex_lock(&si->io_lock);
	switch (cmd) {
	case MYFS_IOC_ZERO_ALL:
		ret = myfs_ioc_zero_all(sb);
		break;
	case MYFS_IOC_ERASE_FS:
		ret = myfs_ioc_erase_fs(sb);
		break;
	case MYFS_IOC_GET_META:
		ret = myfs_ioc_get_meta(sb, arg);
		break;
	case MYFS_IOC_GET_MAP:
		ret = myfs_ioc_get_map(sb, arg);
		break;
	default:
		ret = -ENOTTY;  /* неизвестная команда */
		break;
	}
	mutex_unlock(&si->io_lock);
	return ret;
}

static const struct file_operations myfs_file_ops = {
	.owner          = THIS_MODULE,
	.read           = myfs_read,
	.write          = myfs_write,
	.unlocked_ioctl = myfs_ioctl,
	.compat_ioctl   = myfs_ioctl,
	.llseek         = default_llseek,
};

static const struct inode_operations myfs_file_inode_ops = {
};


/* ===========================================================================
 *  Операции каталога: показываем все файлы в корне
 * ======================================================================== */

/* Вперёд объявим конструктор inode'а для файла. */
static struct inode *myfs_make_file_inode(struct super_block *sb, u64 index);

/*
 * lookup: VFS просит найти запись с именем dentry->d_name в каталоге dir.
 */
static struct dentry *myfs_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct myfs_sb_info *si = sb->s_fs_info;
	const char *name = dentry->d_name.name;
	struct inode *inode = NULL;
	unsigned long idx;

	/* Имя должно иметь вид "fileN". */
	if (strncmp(name, "file", 4) == 0) {
		if (kstrtoul(name + 4, 10, &idx) == 0 && idx < si->file_count)
			inode = myfs_make_file_inode(sb, idx);
	}

	/* d_add привязывает inode (или NULL — «нет такого файла») к dentry. */
	return d_splice_alias(inode, dentry);
}

/*
 * readdir/iterate: перечисляем содержимое корня — служебные "." и ".." плюс
 * по одной записи "fileN" на каждый файл.
 */
static int myfs_iterate(struct file *filp, struct dir_context *ctx)
{
	struct inode *inode = file_inode(filp);
	struct super_block *sb = inode->i_sb;
	struct myfs_sb_info *si = sb->s_fs_info;
	char name[32];
	int len;

	/* dir_emit_dots выдаёт "." и ".." и продвигает ctx->pos на 0/1. */
	if (!dir_emit_dots(filp, ctx))
		return 0;

	/* Файлы нумеруются с ctx->pos==2 (после "." и ".."). */
	while (ctx->pos >= 2 && (u64)(ctx->pos - 2) < si->file_count) {
		u64 idx = ctx->pos - 2;

		len = snprintf(name, sizeof(name), "file%llu",
			       (unsigned long long)idx);
		if (!dir_emit(ctx, name, len,
			      MYFS_FIRST_INO + idx, DT_REG))
			break;
		ctx->pos++;
	}
	return 0;
}

static const struct file_operations myfs_dir_ops = {
	.owner          = THIS_MODULE,
	.iterate_shared = myfs_iterate,
	.read           = generic_read_dir,
	.llseek         = generic_file_llseek,
};

static const struct inode_operations myfs_dir_inode_ops = {
	.lookup = myfs_lookup,
};

/* ===========================================================================
 *  Конструкторы inode'ов
 * ======================================================================== */

/* Создать inode для файла с заданным индексом. */
static struct inode *myfs_make_file_inode(struct super_block *sb, u64 index)
{
	struct myfs_file_header hdr;
	struct inode *inode;
	struct myfs_inode_info *mi;

	inode = new_inode(sb);
	if (!inode)
		return NULL;

	inode->i_ino = MYFS_FIRST_INO + index;
	inode->i_mode = S_IFREG | 0644;
	/* Для обычного файла отдельные inode-операции не нужны: чтение/запись
	 * и ioctl обслуживаются через i_fop. i_op оставляем NULL. */
	inode->i_op = &myfs_file_inode_ops;
	inode->i_fop = &myfs_file_ops;

	/* Время — текущее. */
	simple_inode_init_ts(inode);

	/* Размер inode'а = текущая длина данных файла (читаем заголовок). */
	if (myfs_read_file_header(sb, index, &hdr) == 0)
		i_size_write(inode, hdr.data_len);
	else
		i_size_write(inode, 0);

	mi = MYFS_I(inode);
	mi->file_index = index;

	return inode;
}

/* Создать inode корневого каталога. */
static struct inode *myfs_make_root_inode(struct super_block *sb)
{
	struct inode *inode;

	inode = new_inode(sb);
	if (!inode)
		return NULL;

	inode->i_ino = MYFS_ROOT_INO;
	inode->i_mode = S_IFDIR | 0755;
	inode->i_op = &myfs_dir_inode_ops;
	inode->i_fop = &myfs_dir_ops;
	simple_inode_init_ts(inode);
	set_nlink(inode, 2);

	return inode;
}

/* ===========================================================================
 *  Суперблок: чтение/запись/проверка, инициализация (форматирование)
 * ======================================================================== */

/* Записать суперблок в обе копии (через нашу посекторную запись). */
static int myfs_write_super_copies(struct super_block *sb,
				   struct myfs_super_block *dsb)
{
	struct myfs_sb_info *si = sb->s_fs_info;
	u8 *sector;
	int ret;

	sector = kzalloc(MYFS_SECTOR_SIZE, GFP_KERNEL);
	if (!sector)
		return -ENOMEM;

	/* Пересчитываем контрольную сумму перед записью. */
	dsb->checksum = myfs_sb_checksum(dsb);
	memcpy(sector, dsb, sizeof(*dsb));

	ret = myfs_write_sector(sb, sb_offset, sector);
	if (ret)
		goto out;

	ret = myfs_write_sector(sb, si->sb_copy_offset, sector);
out:
	kfree(sector);
	return ret;
}

/*
 * Прочитать суперблок из конкретного сектора и проверить его целостность
 * (магия, версия, контрольная сумма). Возвращает 0 при успехе.
 */
static int myfs_read_super_at(struct super_block *sb, u64 sector,
			      struct myfs_super_block *out)
{
	u8 *buf;
	struct myfs_super_block *dsb;
	int ret;

	buf = kmalloc(MYFS_SECTOR_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = myfs_read_sector(sb, sector, buf);
	if (ret)
		goto out;

	dsb = (struct myfs_super_block *)buf;

	if (dsb->magic != MYFS_MAGIC) {
		ret = -EINVAL;
		goto out;
	}
	if (dsb->version != MYFS_VERSION) {
		ret = -EINVAL;
		goto out;
	}
	/* Проверка целостности через контрольную сумму. */
	if (dsb->checksum != myfs_sb_checksum(dsb)) {
		ret = -EUCLEAN;  /* данные повреждены */
		goto out;
	}

	*out = *dsb;
	ret = 0;
out:
	kfree(buf);
	return ret;
}

/*
 * Отформатировать устройство: создать суперблок (две копии) и заголовки
 * всех файлов. Вызывается, если на устройстве не найдена валидная ФС.
 */
static int myfs_format(struct super_block *sb)
{
	struct myfs_sb_info *si = sb->s_fs_info;
	struct myfs_super_block dsb;
	struct myfs_file_header hdr;
	u64 idx;
	int ret;

	/* Первый сектор данных идёт после обеих копий суперблока.
	 * Берём максимум, чтобы данные точно не наложились на резервную копию. */
	si->first_data_sector = max_t(u64, sb_offset, si->sb_copy_offset) + 1;

	/* Сколько целых файлов помещается в оставшемся пространстве. */
	if (si->total_sectors <= si->first_data_sector) {
		pr_err("myfs: device too small for any files\n");
		return -ENOSPC;
	}
	si->file_count = (si->total_sectors - si->first_data_sector) /
			 si->file_size_sectors;
	if (si->file_count == 0) {
		pr_err("myfs: device too small (no files fit)\n");
		return -ENOSPC;
	}

	/* Заполняем суперблок. */
	memset(&dsb, 0, sizeof(dsb));
	dsb.magic = MYFS_MAGIC;
	dsb.version = MYFS_VERSION;
	dsb.total_sectors = si->total_sectors;
	dsb.sb_copy_offset = si->sb_copy_offset;
	dsb.max_name_len = si->max_name_len;
	dsb.file_size_sectors = si->file_size_sectors;
	dsb.first_data_sector = si->first_data_sector;
	dsb.file_count = si->file_count;

	ret = myfs_write_super_copies(sb, &dsb);
	if (ret)
		return ret;

	/* Создаём (инициализируем) заголовки всех файлов: пустые данные. */
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = MYFS_MAGIC;
	hdr.data_len = 0;
	hdr.data_checksum = myfs_data_checksum(NULL, 0);

	for (idx = 0; idx < si->file_count; idx++) {
		ret = myfs_write_file_header(sb, idx, &hdr);
		if (ret)
			return ret;
	}

	pr_info("myfs: formatted device, %llu files of %u sectors each\n",
		(unsigned long long)si->file_count, si->file_size_sectors);
	return 0;
}

/* ===========================================================================
 *  Операции суперблока VFS
 * ======================================================================== */

/* Аллокатор нашего inode'а (с встроенным VFS-inode). */
static struct inode *myfs_alloc_inode(struct super_block *sb)
{
	struct myfs_inode_info *mi;

	mi = kmem_cache_alloc(myfs_inode_cachep, GFP_KERNEL);
	if (!mi)
		return NULL;
	mi->file_index = 0;
	inode_init_once(&mi->vfs_inode);
	return &mi->vfs_inode;
}

static void myfs_free_inode(struct inode *inode)
{
	kmem_cache_free(myfs_inode_cachep, MYFS_I(inode));
}

/* Освобождение ресурсов при размонтировании. */
static void myfs_put_super(struct super_block *sb)
{
	struct myfs_sb_info *si = sb->s_fs_info;

	/* Само блочное устройство закрывает kill_block_super() (парная для
	 * mount_bdev). Здесь освобождаем только нашу приватную структуру. */
	kfree(si);
	sb->s_fs_info = NULL;

	pr_info("myfs: unmounted\n");
}

static const struct super_operations myfs_super_ops = {
	.alloc_inode = myfs_alloc_inode,
	.free_inode  = myfs_free_inode,
	.put_super   = myfs_put_super,
	.statfs      = simple_statfs,
	.drop_inode  = generic_delete_inode,
};

/*
 * fill_super: ядро вызывает это при монтировании (через mount_bdev).
 * Имя диска по условию задаётся параметром модуля device_name. Оно же
 * указывается в команде mount, поэтому мы дополнительно сверяем их (если
 * параметр задан) — это удобно для контроля и соответствует требованию
 * «название диска задаётся аргументом модуля».
 */
static int myfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct myfs_sb_info *si;
	struct block_device *bdev = sb->s_bdev;
	struct myfs_super_block dsb_primary, dsb_backup;
	struct inode *root_inode;
	int ret;
	int have_primary = 0, have_backup = 0;
	loff_t dev_bytes;

	/* --- Проверка параметров модуля --- */
	if (max_file_sectors < 1 || max_file_sectors > MYFS_MAX_FILESZ_LIMIT) {
		pr_err("myfs: invalid max_file_sectors=%u\n", max_file_sectors);
		return -EINVAL;
	}
	if (max_name_len < 1 || max_name_len > MYFS_MAX_NAME_LIMIT) {
		pr_err("myfs: invalid max_name_len=%u\n", max_name_len);
		return -EINVAL;
	}
	if (sb_offset == sb_copy_offset) {
		pr_err("myfs: sb_offset and sb_copy_offset must differ\n");
		return -EINVAL;
	}

	si = kzalloc(sizeof(*si), GFP_KERNEL);
	if (!si)
		return -ENOMEM;
	mutex_init(&si->io_lock);
	sb->s_fs_info = si;

	/* Настраиваем размер блока ФС = 512 байт (наш «сектор»).
	 * sb_set_blocksize работает с уже установленным sb->s_bdev. */
	if (!sb_set_blocksize(sb, MYFS_SECTOR_SIZE)) {
		pr_err("myfs: cannot set blocksize %u\n", MYFS_SECTOR_SIZE);
		ret = -EINVAL;
		goto err_free;
	}
	sb->s_magic = MYFS_MAGIC;
	sb->s_op = &myfs_super_ops;
	sb->s_maxbytes = MAX_LFS_FILESIZE;

	/* Размер устройства в секторах. */
	dev_bytes = bdev_nr_bytes(bdev);
	si->total_sectors = (u64)dev_bytes / MYFS_SECTOR_SIZE;

	/* Заполняем геометрию из параметров (на случай форматирования). */
	si->sb_copy_offset = sb_copy_offset;
	si->max_name_len = max_name_len;
	si->file_size_sectors = max_file_sectors;

	/* Проверим, что обе копии суперблока помещаются на устройство. */
	if (sb_offset >= si->total_sectors ||
	    si->sb_copy_offset >= si->total_sectors) {
		pr_err("myfs: superblock offset beyond device size\n");
		ret = -EINVAL;
		goto err_free;
	}

	/* --- Читаем обе копии суперблока с проверкой целостности --- */
	if (myfs_read_super_at(sb, sb_offset, &dsb_primary) == 0)
		have_primary = 1;
	if (myfs_read_super_at(sb, si->sb_copy_offset, &dsb_backup) == 0)
		have_backup = 1;

	if (have_primary || have_backup) {
		/* Найдена существующая ФС. Берём валидную копию; если основная
		 * повреждена, а резервная цела — восстанавливаем основную. */
		struct myfs_super_block *good;

		if (have_primary)
			good = &dsb_primary;
		else
			good = &dsb_backup;

		/* Загружаем геометрию из суперблока. */
		si->total_sectors    = good->total_sectors;
		si->sb_copy_offset   = good->sb_copy_offset;
		si->max_name_len     = good->max_name_len;
		si->file_size_sectors = good->file_size_sectors;
		si->first_data_sector = good->first_data_sector;
		si->file_count       = good->file_count;

		if (!have_primary && have_backup) {
			pr_warn("myfs: primary superblock corrupted, restoring from backup\n");
			ret = myfs_write_super_copies(sb, good);
			if (ret)
				goto err_free;
		} else if (have_primary && !have_backup) {
			pr_warn("myfs: backup superblock corrupted, restoring from primary\n");
			ret = myfs_write_super_copies(sb, good);
			if (ret)
				goto err_free;
		}
		pr_info("myfs: mounted existing FS, %llu files\n",
			(unsigned long long)si->file_count);
	} else {
		/* Валидной ФС нет — форматируем. */
		pr_info("myfs: no valid FS found, formatting device\n");
		ret = myfs_format(sb);
		if (ret)
			goto err_free;
	}

	/* --- Создаём корневой каталог --- */
	root_inode = myfs_make_root_inode(sb);
	if (!root_inode) {
		ret = -ENOMEM;
		goto err_free;
	}
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		/* d_make_root при ошибке сам освобождает root_inode. */
		ret = -ENOMEM;
		goto err_free;
	}

	return 0;

err_free:
	kfree(si);
	sb->s_fs_info = NULL;
	return ret;
}

/*
 * mount: используем mount_bdev — каноничный путь для ФС на блочном устройстве.
 * Он открывает устройство, корректно настраивает buffer cache и s_bdev, после
 * чего вызывает myfs_fill_super. Имя устройства приходит в dev_name (из команды
 * mount). По условию задания имя диска также задаётся параметром модуля
 * device_name; если он задан, считаем его «эталонным» и логируем сверку.
 */
static struct dentry *myfs_mount(struct file_system_type *fs_type,
				 int flags, const char *dev_name, void *data)
{
	if (device_name && device_name[0] != '\0' &&
	    strcmp(device_name, dev_name) != 0) {
		pr_warn("myfs: mount device '%s' differs from module param device_name='%s'\n",
			dev_name, device_name);
	}
	return mount_bdev(fs_type, flags, dev_name, data, myfs_fill_super);
}

static struct file_system_type myfs_fs_type = {
	.owner    = THIS_MODULE,
	.name     = MYFS_FS_NAME,
	.mount    = myfs_mount,
	.kill_sb  = kill_block_super,  /* парная функция для mount_bdev */
	.fs_flags = FS_REQUIRES_DEV,
};

/* ===========================================================================
 *  Инициализация/выгрузка модуля
 * ======================================================================== */

static int __init myfs_init(void)
{
	int ret;

	/* Кеш под inode'ы. */
	myfs_inode_cachep = kmem_cache_create("myfs_inode_cache",
					      sizeof(struct myfs_inode_info),
					      0, SLAB_RECLAIM_ACCOUNT, NULL);
	if (!myfs_inode_cachep)
		return -ENOMEM;

	ret = register_filesystem(&myfs_fs_type);
	if (ret) {
		kmem_cache_destroy(myfs_inode_cachep);
		return ret;
	}

	pr_info("myfs: module loaded (device=%s, sb_offset=%u, sb_copy_offset=%u, "
		"max_name_len=%u, max_file_sectors=%u)\n",
		device_name, sb_offset, sb_copy_offset,
		max_name_len, max_file_sectors);
	return 0;
}

static void __exit myfs_exit(void)
{
	unregister_filesystem(&myfs_fs_type);

	/* Перед уничтожением кеша нужно дождаться завершения RCU-освобождений
	 * inode'ов. */
	rcu_barrier();
	kmem_cache_destroy(myfs_inode_cachep);

	pr_info("myfs: module unloaded\n");
}

module_init(myfs_init);
module_exit(myfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kernel Modules Course");
MODULE_DESCRIPTION("Simple block-device-backed filesystem with dual superblock");
MODULE_VERSION("1.0");
