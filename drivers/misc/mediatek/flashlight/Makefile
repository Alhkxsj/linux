obj-$(CONFIG_MTK_FLASHLIGHT_CORE) += flashlight-core.o flashlight-device.o

# The flash IC drivers live in v4l2/ and include "flashlight-core.h" from
# this directory, so descend into the subdir and put this directory on
# their include path (v4l2/Makefile carries the per-IC obj- lines).
obj-y += v4l2/
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/flashlight
