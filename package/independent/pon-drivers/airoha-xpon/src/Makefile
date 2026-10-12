# The frontend package supplies the shared provider interface.
ccflags-y += -I$(AIROHA_PON_FRONTEND_INCLUDE)

obj-m += airoha-xpon.o
airoha-xpon-y := airoha-xpon-main.o airoha-xpon-identity.o \
	airoha-xpon-xgpon.o airoha-xpon-mode.o \
	airoha-xpon-netlink.o airoha-xpon-ploam.o airoha-xpon-crypto.o \
	airoha-xpon-control.o airoha-xpon-diagnostics.o airoha-xpon-epon.o \
	airoha-xpon-omci.o airoha-xpon-oam.o \
	airoha-xpon-leds.o
