MODULE := math

MODULE_OBJS := \
	angle.o \
	matrix3.o \
	matrix4.o \
	line3d.o \
	line2d.o \
	rect2d.o \
	vector2d.o \
	vector3d.o \
	vector4d.o

# Include common rules
include $(srcdir)/rules.mk
