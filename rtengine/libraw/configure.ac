# Initialization
AC_INIT([LibRaw],[m4_esyscmd(./version.sh)],[info@libraw.org],[],[http://www.libraw.org])
AM_INIT_AUTOMAKE([foreign no-define])
LT_INIT
dnl AM_SILENT_RULES([yes])
AC_CONFIG_MACRO_DIR([m4])
AC_CANONICAL_HOST
PKG_PROG_PKG_CONFIG([0.21])

# Tools to use
AC_PROG_CXX
AC_PROG_CC
LT_INIT([win32-dll])

# Config files to generate

AC_CONFIG_FILES([
	Makefile
	libraw.pc
	libraw_r.pc
])

PC_OPENMP=
PACKAGE_REQUIRES=
PACKAGE_LIBS_PRIVATE=
AC_SUBST([PC_OPENMP])
AC_SUBST([PACKAGE_REQUIRES])
AC_SUBST([PACKAGE_LIBS_PRIVATE])

# check if we want OpenMP support
AC_ARG_ENABLE([openmp],
	[  --enable-openmp    Enable OpenMP support],
	[case "${enableval}" in
		yes) 	openmp=true ;;
		no)		openmp=false ;;
		*)		AC_MSG_ERROR([bad value ${enableval} for --enable-openmp]) ;;
	esac],[openmp=true])

if test x$openmp = xtrue ; then
	AX_OPENMP([openmp_found=true], [openmp_found=false])
	if test x$openmp_found = xtrue ; then
		CXXFLAGS="$CXXFLAGS $OPENMP_CFLAGS"
		CFLAGS="$CFLAGS $OPENMP_CFLAGS"
		PC_OPENMP=" $OPENMP_CFLAGS"
	else
		AC_MSG_WARN([OpenMP support cannot be enabled because your system doesn't support it.])
	fi
fi

# check for libjpeg v8
AC_ARG_ENABLE([jpeg],
	[  --enable-jpeg    Enable JPEG support for Lossy compressed DNG files],
	[case "${enableval}" in
		yes)	jpeg=true ;;
		no)	jpeg=false ;;
		*)	AC_MSG_ERROR([bad value ${enableval} for --enable-jpeg]) ;;
	esac],[jpeg=true])

if test x$jpeg = xtrue; then
        AC_CHECK_LIB([jpeg], [jpeg_mem_src],
                [
                        AC_CHECK_HEADERS([jpeglib.h], [
                                CPPFLAGS="$CPPFLAGS -DUSE_JPEG -DUSE_JPEG8"
                                LIBS="$LIBS -ljpeg"
                                PACKAGE_LIBS_PRIVATE="-ljpeg $PACKAGE_LIBS_PRIVATE"
                        ], [:])
                ],
                [:]
        )
fi

# check if we want zlib support
AC_ARG_ENABLE([zlib],
	[  --enable-zlib    Enable zlib support for deflate compressed DNG files],
	[case "${enableval}" in
		yes)	zlib=true ;;
		no)		zlib=false ;;
		*)		AC_MSG_ERROR([bad value ${enableval} for --enable-zlib]) ;;
	esac],[zlib=true])

if test x$zlib = xtrue; then
	PKG_CHECK_MODULES([ZLIB],[zlib],[
		CPPFLAGS="$CPPFLAGS $ZLIB_CFLAGS -DUSE_ZLIB"
		LIBS="$LIBS $ZLIB_LIBS"
		PACKAGE_REQUIRES=zlib
	],
		[:]
	)
fi

# check if we want LCMS support
AC_ARG_ENABLE([lcms],
	[  --enable-lcms    Enable LCMS support],
	[case "${enableval}" in
		yes)	lcms=true ;;
		no)		lcms=false ;;
		*)		AC_MSG_ERROR([bad value ${enableval} for --enable-lcms]) ;;
	esac],[lcms=true])

if test x$lcms = xtrue; then
	PKG_CHECK_MODULES([LCMS2],[lcms2],[
		CPPFLAGS="$CPPFLAGS $LCMS2_CFLAGS -DUSE_LCMS2"
		LIBS="$LIBS $LCMS2_LIBS"
	        PACKAGE_REQUIRES=lcms2
	],[
	PKG_CHECK_MODULES([LCMS],[lcms],[
		CPPFLAGS="$CPPFLAGS $LCMS_CFLAGS -DUSE_LCMS"
		LIBS="$LIBS $LCMS_LIBS"
	        PACKAGE_REQUIRES=lcms
	],[
		:
	])
	])
fi

# check if we want build examples
AC_ARG_ENABLE([examples],
	[  --enable-examples    Enable building of examples],
	[case "${enableval}" in
		yes) 	examples=true ;;
		no)		examples=false ;;
		*)		AC_MSG_ERROR([bad value ${enableval} for --enable-examples]) ;;
	esac],[examples=true])
AM_CONDITIONAL([EXAMPLES], [test x$examples = xtrue])

LIBS="$LIBS -lm"

case "${host_os}" in
	*mingw32*) LIBS="$LIBS -lws2_32" ;;
esac

AC_SUBST([LIBRAW_SHLIB_VERSION],m4_esyscmd([./shlib-version.sh]))
AC_SUBST([LIBRAW_RELEASE_VERSION],m4_esyscmd([./version.sh]))

AC_OUTPUT
