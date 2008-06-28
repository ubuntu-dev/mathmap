# If you want MathMap to provide a command line interface as well,
# uncomment the following line.  Note that compiling it requires
# libjpeg, libpng and giflib.
CMDLINE = YES

# Prefix of your GIMP binaries.  Usually you can leave this line
# commented.  If you have more than one GIMP versions installed, you
# should give the prefix for the one which you want to build MathMap
# for.
#GIMP_BIN = /usr/bin/

# Choose which gif library you have.
GIFLIB = -lgif
#GIFLIB = -lungif

# If you are building on MacOS X, uncomment the following line
#MACOSX = YES

# If you want the final GIMP rendering to be multi-threaded then
# uncomment the following line
#THREADED = -DTHREADED_FINAL_RENDER

# If want to have movie (Quicktime) support in the command line,
# uncomment the following line.
#MOVIES = YES

# If you do not want localization (relevant for plug-in version only),
# comment the following line
ENABLE_NLS = YES

# The settings for the following directories doesn't affect anything
# because MathMap cannot install system-wide yet.

# Prefix for the software installation
PREFIX = /usr/local

# Directory where the localization files should be installed in
LOCALEDIR = $(PREFIX)/share/locale

# You should not need to change anything beyond this line.
# -------------------------------------------------------

VERSION = 1.3.3

#OPT_CFLAGS := -O2
OPT_CFLAGS := -g -DDEBUG_OUTPUT

ifeq ($(MACOSX),YES)
CGEN_CC=-DCGEN_CC="\"cc -O2 -c -fPIC -faltivec -o\""
CGEN_LD=-DCGEN_LD="\"cc -bundle -flat_namespace -undefined suppress -o\""
MACOSX_LIBS=-lmx
MACOSX_CFLAGS=-I/sw/include
else
CGEN_CC=-DCGEN_CC="\"gcc -O2 -c -fPIC -o\""
CGEN_LD=-DCGEN_LD="\"gcc -shared -o\""
endif

CGEN_CFLAGS=$(CGEN_CC) $(CGEN_LD)
#CGEN_LDFLAGS=-Wl,--export-dynamic

GIMPTOOL := $(GIMP_BIN)gimptool-2.0
GIMPDIR := .gimp-$(basename $(shell $(GIMPTOOL) --version))
GIMPDATADIR := `$(GIMPTOOL) --gimpdatadir`
GIMP_CFLAGS := `$(GIMPTOOL) --cflags` `pkg-config --cflags gmodule-2.0 gthread-2.0 gtksourceview-1.0 libgnomecanvas-2.0`
GIMP_LDFLAGS := `$(GIMPTOOL) --libs` `pkg-config --libs gmodule-2.0 gthread-2.0 gtksourceview-1.0 libgnomecanvas-2.0`

TEMPLATE_DIR = $(GIMPDATADIR)/mathmap
PIXMAP_DIR = $(GIMPDATADIR)/mathmap

CFLAGS = -I. -D_GNU_SOURCE $(CGEN_CFLAGS) $(OPT_CFLAGS) -Wall $(GIMP_CFLAGS) -DLOCALEDIR=\"$(LOCALEDIR)\" -DTEMPLATE_DIR=\"$(TEMPLATE_DIR)\" -DPIXMAP_DIR=\"$(PIXMAP_DIR)\" $(NLS_CFLAGS) $(MACOSX_CFLAGS) -DUSE_PTHREADS $(THREADED)
LDFLAGS = $(GIMP_LDFLAGS) $(MACOSX_LIBS) -lm -lgsl -lgslcblas

ifeq ($(MOVIES),YES)
CFLAGS += -I/usr/local/include/quicktime -DMOVIES
LDFLAGS += -lquicktime -lpthread
endif

ifeq ($(CMDLINE),YES)
CMDLINE_OBJECTS = mathmap_cmdline.o getopt.o getopt1.o generators/blender/blender.o
CMDLINE_LIBS = rwimg/librwimg.a
CMDLINE_TARGETS = librwimg
FORMATDEFS = -DRWIMG_JPEG -DRWIMG_PNG -DRWIMG_GIF
CFLAGS += -DMATHMAP_CMDLINE -DGIMPDATADIR=\"$(GIMPDATADIR)\"
LDFLAGS += -ljpeg -lpng $(GIFLIB)
endif

ifeq ($(ENABLE_NLS),YES)
NLS_CFLAGS = -DENABLE_NLS
MOS = fr.mo
endif

CFLAGS += -DMATHMAP_VERSION=\"$(VERSION)\"

CC = gcc

export CFLAGS CC FORMATDEFS

COMMON_OBJECTS = mathmap_common.o builtins.o exprtree.o parser.o scanner.o vars.o tags.o tuples.o internals.o macros.o userval.o overload.o jump.o noise.o spec_func.o compiler.o bitvector.o expression_db.o drawable.o floatmap.o designer/designer.o designer/cycles.o designer/widget.o designer/loadsave.o designer_filter.o native-filters/gauss.o compopt/dce.o compopt/resize.o

GIMP_OBJECTS = mathmap.o

OBJECTS = $(COMMON_OBJECTS) $(CMDLINE_OBJECTS) $(GIMP_OBJECTS)

mathmap : compiler_types.h $(OBJECTS) $(CMDLINE_TARGETS) liblispreader
	$(CC) $(CGEN_LDFLAGS) -o mathmap $(OBJECTS) $(CMDLINE_LIBS) lispreader/liblispreader.a $(LDFLAGS)

librwimg :
	$(MAKE) -C rwimg

liblispreader :
	$(MAKE) -C lispreader -f Makefile.dist

#compiler_test : $(COMMON_OBJECTS) compiler_test.o
#	$(CC) $(CGEN_LDFLAGS) -o compiler_test $(COMMON_OBJECTS) compiler_test.o $(LDFLAGS) -lgsl -lgslcblas

%.o : %.c
	$(CC) $(CFLAGS) $(FORMATDEFS) -o $@ -c $<

%.mo : %.po
	msgfmt -o $@ $<

parser.c parser.h : parser.y
	bison -d parser.y
	mv parser.tab.c parser.c
	mv parser.tab.h parser.h

scanner.c : scanner.fl parser.h
	flex scanner.fl
	mv lex.yy.c scanner.c

compiler.o : new_builtins.c opdefs.h opfuncs.h compiler_types.h

new_builtins.c opdefs.h opfuncs.h compiler_types.h : builtins.lisp ops.lisp
	clisp builtins.lisp

blender.o : generators/blender/blender.c

install : mathmap
	$(GIMPTOOL) --install-admin-bin mathmap
	if [ ! -d $(GIMPDATADIR)/mathmap ] ; then mkdir $(GIMPDATADIR)/mathmap ; fi
	cp new_template.c $(GIMPDATADIR)/mathmap/
	cp opmacros.h $(GIMPDATADIR)/mathmap/
	cp lispreader/pools.h $(GIMPDATADIR)/mathmap/
	cp generators/blender/blender_template.c $(GIMPDATADIR)/mathmap/
	cp generators/blender/blender_opmacros.h $(GIMPDATADIR)/mathmap/
	if [ ! -d $(GIMPDATADIR)/mathmap/expressions ] ; then cp -r examples $(GIMPDATADIR)/mathmap/expressions ; fi
	if [ ! -d $(PIXMAP_DIR) ] ; then mkdir $(PIXMAP_DIR) ; fi
	cp pixmaps/*.png $(PIXMAP_DIR)

install-local : mathmap
#	if [ ! -d $(TEMPLATE_DIR) ] ; then mkdir $(TEMPLATE_DIR) ; fi
#	cp generators/blender/blender_template.c generators/blender/blender_opmacros.h $(TEMPLATE_DIR)

	if [ ! -d $(HOME)/$(GIMPDIR) ] ; then mkdir $(HOME)/$(GIMPDIR) ; fi
	if [ ! -d $(HOME)/$(GIMPDIR)/plug-ins ] ; then mkdir $(HOME)/$(GIMPDIR)/plug-ins ; fi
	cp mathmap $(HOME)/$(GIMPDIR)/plug-ins/

	if [ ! -d $(HOME)/$(GIMPDIR)/mathmap ] ; then mkdir $(HOME)/$(GIMPDIR)/mathmap ; fi
	cp new_template.c $(HOME)/$(GIMPDIR)/mathmap/
	cp opmacros.h $(HOME)/$(GIMPDIR)/mathmap/

	if [ ! -d $(HOME)/$(GIMPDIR)/mathmap/expressions ] ; then cp -r examples $(HOME)/$(GIMPDIR)/mathmap/expressions ; fi

	if [ ! -d $(HOME)/.gnome2/gtksourceview-1.0 ] ; then mkdir $(HOME)/.gnome2/gtksourceview-1.0 ; fi
	if [ ! -d $(HOME)/.gnome2/gtksourceview-1.0/language-specs ] ; then mkdir $(HOME)/.gnome2/gtksourceview-1.0/language-specs ; fi
	cp mathmap.lang $(HOME)/.gnome2/gtksourceview-1.0/language-specs

install-mos : $(MOS)
	if [ ! -d $(LOCALEDIR)/fr ] ; then mkdir $(LOCALEDIR)/fr ; fi
	if [ ! -d $(LOCALEDIR)/fr/LC_MESSAGES ] ; then mkdir $(LOCALEDIR)/fr/LC_MESSAGES ; fi
	cp fr.mo $(LOCALEDIR)/fr/LC_MESSAGES/mathmap.mo

clean :
	rm -f *.o designer/*.o native-filters/*.o compopt/*.o generators/blender/*.o mathmap compiler parser.output core
	find . -name '*~' | xargs -r -d '\n' rm
	$(MAKE) -C rwimg clean
	$(MAKE) -C lispreader clean
	rm -rf debian/mathmap debian/mathmap.substvars

realclean : clean
	rm -f new_builtins.c opdefs.h opfuncs.h compiler_types.h scanner.c parser.[ch] .nfs* mathmap-*.tar.gz

TAGS :
	etags `find . -name '*.c' -o -name '*.h' -o -name '*.lisp'`

dist : new_builtins.c parser.c scanner.c clean
	rm -rf mathmap-$(VERSION)
	mkdir mathmap-$(VERSION)
	cp Makefile README README.blender README.filters README.mercurial ANNOUNCEMENT COPYING INSTALL mathmap.spec *.[ch] builtins.lisp ops.lisp parser.y scanner.fl *.po mathmap.lang mathmap-$(VERSION)
	cp -r debian mathmap-$(VERSION)
	mkdir mathmap-$(VERSION)/lisp-utils
	cp lisp-utils/*.lisp mathmap-$(VERSION)/lisp-utils
	mkdir mathmap-$(VERSION)/generators
	mkdir mathmap-$(VERSION)/generators/blender
	cp generators/blender/blender.[ch] generators/blender/blender_template.c generators/blender/blender_opmacros.h generators/blender/make_some_plugins mathmap-$(VERSION)/generators/blender
	mkdir mathmap-$(VERSION)/designer
	cp designer/*.[ch] mathmap-$(VERSION)/designer
	mkdir mathmap-$(VERSION)/native-filters
	cp native-filters/*.[ch] mathmap-$(VERSION)/native-filters
	mkdir mathmap-$(VERSION)/compopt
	cp compopt/*.[ch] mathmap-$(VERSION)/compopt
	mkdir mathmap-$(VERSION)/doc
	cp html/language.html html/reference.html html/cartesian.png html/gray_gradient.jpg html/finn.jpg html/sinegraph.png html/sine_finn.jpg html/polar.png html/finn_pond.jpg html/target.jpg html/rmod.jpg html/finn_vignette.jpg html/redgreengradient.jpg html/noise.jpg mathmap-$(VERSION)/doc
	mkdir mathmap-$(VERSION)/pixmaps
	cp pixmaps/*.png mathmap-$(VERSION)/pixmaps
	cp -rL examples lispreader rwimg mathmap-$(VERSION)/
	rm -rf `find mathmap-$(VERSION) -name '.svn'`
	rm -rf `find mathmap-$(VERSION) -name '.hg*'`
	touch mathmap-$(VERSION)/parser.[ch] mathmap-$(VERSION)/scanner.c mathmap-$(VERSION)/new_builtins.c mathmap-$(VERSION)/opdefs.h mathmap-$(VERSION)/opfuncs.h mathmap-$(VERSION)/compiler_types.h
	tar -zcvf mathmap-$(VERSION).tar.gz mathmap-$(VERSION)
	rm -rf mathmap-$(VERSION)
