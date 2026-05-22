// SPDX-License-Identifier: GPL-2.0
//
// myfs_tool.cpp — userspace-утилита для демонстрации работы файловой
// системы MYFS.
//
// Возможности:
//   1) Режим демонстрации (test): обходит ВСЕ файлы смонтированной ФС,
//      в каждый пишет случайное 64-битное число, затем читает его обратно
//      и сверяет. Печатает сводку.
//   2) CLI для вызова ioctl модуля:
//        - zero      — обнулить все файлы (MYFS_IOC_ZERO_ALL)
//        - erase     — стереть ФС / суперблоки (MYFS_IOC_ERASE_FS)
//        - meta      — получить метаинформацию (хеши) всех файлов
//        - map N     — получить маппинг секторов для файла с индексом N
//
// IOCTL вызываются на любом открытом файле ФС (для map передаётся индекс
// нужного файла аргументом структуры). Программа берёт первый доступный
// файл в каталоге монтирования как «ручку» для ioctl.


#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <random>
#include <string>
#include <vector>
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

// Берём те же определения структур и команд ioctl, что и в модуле.
// Файл myfs.h спроектирован так, чтобы корректно компилироваться в userspace.
#include "myfs.h"

namespace {

// Возвращает отсортированный список путей ко всем файлам "fileN" в каталоге.
std::vector<std::string> list_files(const std::string &mount_dir)
{
	std::vector<std::pair<long, std::string>> items;
	DIR *d = opendir(mount_dir.c_str());
	if (!d) {
		perror("opendir");
		return {};
	}

	struct dirent *de;
	while ((de = readdir(d)) != nullptr) {
		const std::string name = de->d_name;
		// Интересуют только записи вида "fileN".
		if (name.rfind("file", 0) == 0) {
			char *end = nullptr;
			long idx = strtol(name.c_str() + 4, &end, 10);
			if (end && *end == '\0')
				items.emplace_back(idx, mount_dir + "/" + name);
		}
	}
	closedir(d);

	// Сортируем по индексу, чтобы обход был детерминированным.
	std::sort(items.begin(), items.end(),
		  [](const auto &a, const auto &b) { return a.first < b.first; });

	std::vector<std::string> result;
	result.reserve(items.size());
	for (auto &it : items)
		result.push_back(it.second);
	return result;
}

// Открыть первый попавшийся файл ФС — используется как «ручка» для ioctl.
int open_any_file(const std::string &mount_dir)
{
	auto files = list_files(mount_dir);
	if (files.empty()) {
		fprintf(stderr, "No files found in %s\n", mount_dir.c_str());
		return -1;
	}
	int fd = open(files.front().c_str(), O_RDWR);
	if (fd < 0)
		perror("open");
	return fd;
}

// --- Режим демонстрации: запись и чтение случайного числа в каждом файле ---
int run_test(const std::string &mount_dir)
{
	auto files = list_files(mount_dir);
	if (files.empty()) {
		fprintf(stderr, "No files found in %s\n", mount_dir.c_str());
		return 1;
	}

	std::mt19937_64 rng(std::random_device{}());
	size_t ok = 0, fail = 0;

	printf("Found %zu files in %s\n", files.size(), mount_dir.c_str());

	for (size_t i = 0; i < files.size(); ++i) {
		const std::string &path = files[i];

		// Генерируем случайное 64-битное число.
		uint64_t value = rng();

		// --- Запись ---
		int fd = open(path.c_str(), O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "open(%s) for write failed: %s\n",
				path.c_str(), strerror(errno));
			++fail;
			continue;
		}
		lseek(fd, 0, SEEK_SET);
		ssize_t w = write(fd, &value, sizeof(value));
		if (w != (ssize_t)sizeof(value)) {
			fprintf(stderr, "write(%s) failed: %s\n",
				path.c_str(), strerror(errno));
			close(fd);
			++fail;
			continue;
		}
		close(fd);

		// --- Чтение обратно ---
		uint64_t readback = 0;
		fd = open(path.c_str(), O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "open(%s) for read failed: %s\n",
				path.c_str(), strerror(errno));
			++fail;
			continue;
		}
		lseek(fd, 0, SEEK_SET);
		ssize_t r = read(fd, &readback, sizeof(readback));
		close(fd);

		if (r != (ssize_t)sizeof(readback)) {
			fprintf(stderr, "read(%s) failed: %s\n",
				path.c_str(), strerror(errno));
			++fail;
			continue;
		}

		// --- Сверка ---
		if (readback == value) {
			++ok;
			// Печатаем подробности только для первых нескольких файлов,
			// чтобы не засорять вывод на больших дисках.
			if (i < 8)
				printf("  [%s] wrote=%llu read=%llu OK\n",
				       path.c_str(),
				       (unsigned long long)value,
				       (unsigned long long)readback);
		} else {
			++fail;
			fprintf(stderr, "  [%s] MISMATCH wrote=%llu read=%llu\n",
				path.c_str(),
				(unsigned long long)value,
				(unsigned long long)readback);
		}
	}

	printf("\nResult: %zu OK, %zu FAILED out of %zu files\n",
	       ok, fail, files.size());
	return fail == 0 ? 0 : 1;
}

// --- IOCTL: обнулить все файлы ---
int ioctl_zero(const std::string &mount_dir)
{
	int fd = open_any_file(mount_dir);
	if (fd < 0)
		return 1;
	int ret = ioctl(fd, MYFS_IOC_ZERO_ALL);
	close(fd);
	if (ret < 0) {
		perror("ioctl ZERO_ALL");
		return 1;
	}
	printf("All files zeroed.\n");
	return 0;
}

// --- IOCTL: стереть ФС ---
int ioctl_erase(const std::string &mount_dir)
{
	int fd = open_any_file(mount_dir);
	if (fd < 0)
		return 1;
	int ret = ioctl(fd, MYFS_IOC_ERASE_FS);
	close(fd);
	if (ret < 0) {
		perror("ioctl ERASE_FS");
		return 1;
	}
	printf("Filesystem erased (superblocks wiped). Unmount to take effect.\n");
	return 0;
}

// --- IOCTL: получить метаинформацию (хеши) всех файлов ---
int ioctl_meta(const std::string &mount_dir)
{
	int fd = open_any_file(mount_dir);
	if (fd < 0)
		return 1;

	// Сначала узнаём, сколько файлов: запросим с заведомо большим запасом.
	// Здесь мы просто берём число файлов из каталога как верхнюю границу.
	auto files = list_files(mount_dir);
	std::vector<struct myfs_meta_entry> entries(files.size());

	struct myfs_meta_list req;
	memset(&req, 0, sizeof(req));
	req.capacity = entries.size();
	req.count = 0;
	req.entries_ptr = reinterpret_cast<uint64_t>(entries.data());

	int ret = ioctl(fd, MYFS_IOC_GET_META, &req);
	close(fd);
	if (ret < 0) {
		perror("ioctl GET_META");
		return 1;
	}

	printf("Metadata for %llu files:\n", (unsigned long long)req.count);
	printf("%-8s %-12s %-12s\n", "index", "data_len", "checksum");
	for (uint64_t i = 0; i < req.count; ++i) {
		printf("%-8llu %-12u 0x%08x\n",
		       (unsigned long long)entries[i].file_index,
		       entries[i].data_len,
		       entries[i].data_checksum);
	}
	return 0;
}

// --- IOCTL: получить маппинг секторов для файла с индексом index ---
int ioctl_map(const std::string &mount_dir, uint64_t index)
{
	int fd = open_any_file(mount_dir);
	if (fd < 0)
		return 1;

	struct myfs_sector_map req;
	memset(&req, 0, sizeof(req));
	req.file_index = index;

	int ret = ioctl(fd, MYFS_IOC_GET_MAP, &req);
	close(fd);
	if (ret < 0) {
		perror("ioctl GET_MAP");
		return 1;
	}

	printf("File %llu sector mapping:\n", (unsigned long long)index);
	printf("  first_sector = %llu\n", (unsigned long long)req.first_sector);
	printf("  sector_count = %llu\n", (unsigned long long)req.sector_count);
	printf("  last_sector  = %llu\n",
	       (unsigned long long)(req.first_sector + req.sector_count - 1));
	return 0;
}

void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s <mount_dir> test           Write+read random numbers to all files\n"
		"  %s <mount_dir> zero           ioctl: zero all files\n"
		"  %s <mount_dir> erase          ioctl: erase filesystem (wipe superblocks)\n"
		"  %s <mount_dir> meta           ioctl: list metadata (checksums) of all files\n"
		"  %s <mount_dir> map <index>    ioctl: get sector mapping of file <index>\n",
		prog, prog, prog, prog, prog);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3) {
		usage(argv[0]);
		return 2;
	}

	const std::string mount_dir = argv[1];
	const std::string cmd = argv[2];

	if (cmd == "test")
		return run_test(mount_dir);
	if (cmd == "zero")
		return ioctl_zero(mount_dir);
	if (cmd == "erase")
		return ioctl_erase(mount_dir);
	if (cmd == "meta")
		return ioctl_meta(mount_dir);
	if (cmd == "map") {
		if (argc < 4) {
			usage(argv[0]);
			return 2;
		}
		uint64_t index = strtoull(argv[3], nullptr, 10);
		return ioctl_map(mount_dir, index);
	}

	usage(argv[0]);
	return 2;
}
