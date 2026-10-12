# SPDX-License-Identifier: GPL-2.0-only
#
# GN28L95 and UX3363 share one I2C owner while exposing separate chip operations.
ccflags-y += -I$(src)
obj-m += airoha-pon-frontend-core.o
obj-m += airoha-en7572.o
obj-m += airoha-paged-bosa.o
airoha-paged-bosa-y := airoha-paged-bosa-main.o \
	airoha-paged-bosa-gn28l95.o \
	airoha-paged-bosa-ux3363.o
