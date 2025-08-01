# Define autoconf style directory variables, for Libreswan.
#
# Copyright (C) 2015, 2021  Andrew Cagney
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.

# force all: as default
.PHONY: all
all:

# From a source directory Makefile use:
#
#   ifndef top_srcdir
#   include ../../mk/dirs.mk
#   endif
#
# (Since the Makefile is included from the build (OBJDIR) directory
# where the relative paths are different and dirs.mk has already been
# included, a guard is needed.)
#
# From a generated build (OBJDIR) directory Makefile use:
#
#   include ../../../mk/dirs.mk
#

# To help testing there is the target ".dirs.mk".  This will print out
# all defined variables.  For instance:
#
#   ( cd . && make -f mk/dirs.mk .dirs.mk )
#   ( cd programs && make -f ../mk/dirs.mk .dirs.mk )
#   ( cd programs/pluto && make -f ../../mk/dirs.mk .dirs.mk )
#
#   ( cd OBJ.* && make -f ../mk/dirs.mk .dirs.mk )
#   ( cd OBJ.*/programs && make -f ../../mk/dirs.mk .dirs.mk )
#   ( cd OBJ.*/programs/pluto && make -f ../../../mk/dirs.mk .dirs.mk )
#


# Check for double include of mk/dirs.mk.  This, as they say, will
# never happen.
#
# Unfortunately, given the presence of this test, you can guess that
# it has.  Some broken Makefile code was effectively executing:
#
#      cd $(abs_top_builddir) && OBJDIR=$(abs_top_builddir) make ...
#
# (i.e., a totally bogus OBJDIR was being pushed into the child make's
# environment).  Either by luck, or poor design, the generated OBJDIR
# Makefiles would then "fix" OBJDIR forcing it to a correct value.
# Remove the "fix" and chaos ensues.
ifeq ($(.dirs.mk),)
.dirs.mk := $(MAKEFILE_LIST)
else
$(warning mk/dirs.mk included twice)
$(warning first include MAKEFILE_LIST: $(.dirs.mk))
$(warning second include MAKEFILE_LIST: $(MAKEFILE_LIST))
$(warning dirs.mk.in.srcdir: $(dirs.mk.in.srcdir))
$(warning srcdir: $(srcdir))
$(warning builddir: $(builddir))
$(warning OBJDIR: $(OBJDIR))
$(warning cwd: $(abspath .))
$(error this will never happen ...)
endif


#
# Determine top_srcdir
#
# The last item in the GNU make variable MAKEFILE_LIST is the relative
# path to the most recent included file (i.e., this file - dirs.mk).
# Since the location of dirs.mk is known relative to top_srcdir,
# top_srcdir can be determined.  These variables are "simply expanded"
# so that they capture the current value.  For more information see
# MAKEFILE_LIST and "simply expanded variables" in "info make".
# Extract the relative path to this file
dirs.mk.file := $(lastword $(MAKEFILE_LIST))
# Convert the relative path to this file into a relative path to this
# file's directory.  Since $(dir) appends a trailing / (such as "mk/"
# or "../mk/") that needs to be stripped.
dirs.mk.dir := $(patsubst %/,%,$(dir $(dirs.mk.file)))
# Finally, drop the mk/ sub-directory.  Again, since $(dir) appends a
# trailing / (such as "./" or "../") that needs to be stripped.
top_srcdir := $(patsubst %/,%,$(dir $(dirs.mk.dir)))
abs_top_srcdir := $(abspath $(top_srcdir))


# Pull in sufficient stuff to get a definition of OBJDIR.  It might be
# set by local includes so pull that in first.
include $(top_srcdir)/mk/local.mk
include $(top_srcdir)/mk/objdir.mk


srcdir := .
abs_srcdir := $(abspath $(srcdir))

ifeq ($(patsubst /%,/,$(OBJDIR)),/)
  # absolute
  top_builddir := $(OBJDIR)
else ifeq ($(top_srcdir),.)
  # avoid ./OBJDIR
  top_builddir := $(OBJDIR)
else
  top_builddir := $(top_srcdir)/$(OBJDIR)
endif
abs_top_builddir := $(abspath $(top_builddir))

# Path down from $(top_srcdir); Note that for non-top-level
# directories PATH_SRCDIR contains a leading / so that ${builddir} is
# built correctly.
path_srcdir := $(subst $(abs_top_srcdir),,$(abs_srcdir))
builddir := $(top_builddir)$(path_srcdir)
abs_builddir := $(abspath $(builddir))

# Always include the other directory in the search path.
#
# XXX: The VPATH+=$(srcdir) will hopefully go away and this will
# become unconditional.
#
# GMAKE looks along $(VPATH) when trying to resolve rules like
# %.o:%.c.

VPATH += $(builddir)
VPATH += $(srcdir)

# For compatibility with existing include files:
LIBRESWANSRCDIR?=$(abs_top_srcdir)
SRCDIR?=$(abs_srcdir)/
OBJDIRTOP?=$(abs_top_builddir)

# Dot targets are never the default.
.PHONY: .dirs.mk
.dirs.mk:
	@echo ""
	@echo For debugging:
	@echo ""
	@echo dirs.mk.file=$(dirs.mk.file)
	@echo dirs.mk.dir=$(dirs.mk.dir)
	@echo path_srcdir=$(path_srcdir)
	@echo top_srcdir=$(top_srcdir)
	@echo dirs.mk.included.from.srcdir="$(dirs.mk.included.from.srcdir)"
	@echo ""
	@echo Relative paths:
	@echo ""
	@echo srcdir=$(srcdir)
	@echo top_srcdir=$(top_srcdir)
	@echo builddir=$(builddir)
	@echo top_builddir=$(top_builddir)
	@echo ""
	@echo Absolute paths:
	@echo ""
	@echo abs_srcdir=$(abs_srcdir)
	@echo abs_top_srcdir=$(abs_top_srcdir)
	@echo abs_builddir=$(abs_builddir)
	@echo abs_top_builddir=$(abs_top_builddir)
	@echo ""
	@echo Backward compatibility:
	@echo ""
	@echo SRCDIR=$(SRCDIR)
	@echo OBJDIRTOP=$(OBJDIRTOP)
	@echo LIBRESWANSRCDIR=$(LIBRESWANSRCDIR)
