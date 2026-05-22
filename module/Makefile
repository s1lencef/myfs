# Makefile для сборки модуля ядра MYFS.
#
# Имя итогового объекта модуля. obj-m означает «собрать как модуль».
obj-m := myfs.o

# Каталог с исходниками/заголовками текущего ядра.
KDIR ?= /lib/modules/$(shell uname -r)/build

# Текущий каталог (где лежат исходники модуля).
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
