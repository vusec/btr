obj-m += btr_help_module.o

btr_help_module-objs := btr_help.o snippet.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
