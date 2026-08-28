# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the linux pstorefs routines.
#

obj-$(CONFIG_PSTORE) += pstore.o

pstore-objs += inode.o platform.o
pstore-$(CONFIG_PSTORE_FTRACE)	+= ftrace.o

pstore-$(CONFIG_PSTORE_PMSG)	+= pmsg.o
<<<<<<< HEAD
pstore-$(CONFIG_PANIC_DISK_STORE)	+= panic_disk_store.o
=======
>>>>>>> main

ramoops-objs += ram.o ram_core.o
obj-$(CONFIG_PSTORE_RAM)	+= ramoops.o
