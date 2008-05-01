#
# Include the TEA standard macro set
#

builtin(include,tclconfig/tcl.m4)

#
# Add here whatever m4 macros you want to define for your package
#

dnl Helper macros
AC_DEFUN([TEAX_LAPPEND], [$1="[$]{$1} $2"])
AC_DEFUN([TEAX_FOREACH], [for $1 in $2; do $3; done])
AC_DEFUN([TEAX_IFEQ], [AS_IF([test "x$1" = "x$2"], [$3])])
AC_DEFUN([TEAX_IFNEQ], [AS_IF([test "x$1" != "x$2"], [$3])])
AC_DEFUN([TEAX_SWITCH], [case "$1" in TEAX_SWITCH_Cases(m4_shift($@)) esac])
AC_DEFUN([TEAX_SWITCH_Cases], [m4_if([$#],0,,[$#],1,,[TEAX_SWITCH_OneCase($1,$2)TEAX_SWITCH_Cases(m4_shift(m4_shift($@)))])])
AC_DEFUN([TEAX_SWITCH_OneCase],[ $1) $2;;])
AC_DEFUN([CygPath],[`${CYGPATH} $1`])

dnl Interesting macros
AC_DEFUN([TEAX_SUBST_RESOURCE], [
    AC_REQUIRE([TEA_CONFIG_CFLAGS])dnl
    TEAX_IFEQ($TEA_PLATFORM, windows, [
	AC_CHECK_PROGS(RC_, 'windres -o' 'rc -nologo -fo', none)
	TEAX_SWITCH($RC_,
	    windres*, [
		rcdef_inc="--include "
		rcdef_start="--define "
		rcdef_q='\"'
		AC_SUBST(RES_SUFFIX, [res.o])
		TEAX_LAPPEND(PKG_OBJECTS, ${PACKAGE_NAME}.res.o)],
	    rc*, [
		rcdef_inc="-i "
		rcdef_start="-d "
		rcdef_q='"'
		AC_SUBST(RES_SUFFIX, [res])
		TEAX_LAPPEND(PKG_OBJECTS, ${PACKAGE_NAME}.res)],
	    *, [
		AC_MSG_WARN([could not find resource compiler])
		RC_=: ])])
    # This next line is because of the brokenness of TEA...
    AC_SUBST(RC, $RC_)
    TEAX_FOREACH(i, $1, [
	TEAX_LAPPEND(RES_DEFS, ${rcdef_inc}\"CygPath($i)\")])
    TEAX_FOREACH(i, $2, [
	TEAX_LAPPEND(RES_DEFS, ${rcdef_start}$i='${rcdef_q}\$($i)${rcdef_q}')])
    AC_SUBST(RES_DEFS)])
AC_DEFUN([TEAX_ADD_PRIVATE_HEADERS], [
    TEAX_FOREACH(i, $@, [
	# check for existence, be strict because it should be present!
	AS_IF([test ! -f "${srcdir}/$i"], [
	    AC_MSG_ERROR([could not find header file '${srcdir}/$i'])])
	TEAX_LAPPEND(PKG_PRIVATE_HEADERS, $i)])
    AC_SUBST(PKG_PRIVATE_HEADERS)])

dnl Extra magic to make things work with Vista and VC
AC_DEFUN([TEAX_VC_MANIFEST], [
    AS_IF([test "$GCC" != yes \
	    -a ${TEA_PLATFORM} == "windows" \
	    -a "${SHARED_BUILD}" = "1"], [
	# This refers to "Manifest Tool" not "Magnetic Tape utility"
	AC_CHECK_PROGS(MT, mt, none)
	AS_IF([test "$MT" != none], [
	    ADD_MANIFEST="${MT} -nologo -manifest [\$]@.manifest -outputresource:[\$]@\;2"
	    AC_SUBST(ADD_MANIFEST)
	    CLEANFILES="$CLEANFILES ${PKG_LIB_FILE}.manifest"])])])

AC_DEFUN([TEAX_SDX], [
    AC_PATH_PROG(SDX, sdx, none)
    TEAX_IFEQ($SDX, none, [
	AC_PATH_PROG(SDX_KIT, sdx.kit, none)
	TEAX_IFNEQ($SDX_KIT, none, [
	    # We assume that sdx.kit is on the path, and that the default
	    # tclsh is activetcl
	    SDX="tclsh '${SDX_KIT}'"])])
    TEAX_IFEQ($SDX, none, [
	AC_MSG_WARN([cannot find sdx; building starkits will fail])
	AC_MSG_NOTICE([building as a normal library still supported])])])
dnl TODO: Adapt this for OSX Frameworks...
dnl This next bit is a bit ugly, but it makes things for tclooConfig.sh...
AC_DEFUN([TEAX_INCLUDE_LINE], [
    eval "$1=\"-I[]CygPath($2)\""
    AC_SUBST($1)])
AC_DEFUN([TEAX_LINK_LINE], [
    AS_IF([test ${TCL_LIB_VERSIONS_OK} = nodots], [
	eval "$1=\"-L[]CygPath($2) -l$3${TCL_TRIM_DOTS}\""
    ], [
	eval "$1=\"-L[]CygPath($2) -l$3${PACKAGE_VERSION}\""
    ])
    AC_SUBST($1)])

dnl +------------------------------------------------------------------------
dnl | TEAX_PATH_TCLOOCONFIG --
dnl |
dnl |	Locate the tclooConfig.sh file
dnl |
dnl | Arguments:
dnl |	none
dnl |
dnl | Results:
dnl |
dnl |	Adds the following arguments to configure:
dnl |		--with-tcloo=...
dnl |
dnl |	Defines the following vars:
dnl |		TCLOO_BIN_DIR	Full path to the directory containing
dnl |				the tclooConfig.sh file
dnl +------------------------------------------------------------------------

AC_DEFUN([TEAX_PATH_TCLOOCONFIG], [
    #
    # Ok, lets find the tclOO configuration
    # First, look for one uninstalled.
    # the alternative search directory is invoked by --with-tcloo
    #

    if test x"${no_tcloo}" = x ; then
	# we reset no_tcloo in case something fails here
	no_tcloo=true
	AC_ARG_WITH(tcloo,
	    AC_HELP_STRING([--with-tcloo],
		[directory containing TclOO configuration (tclooConfig.sh)]),
	    with_tclooconfig=${withval})
	AC_MSG_CHECKING([for TclOO configuration])
	AC_CACHE_VAL(ac_cv_c_tclooconfig,[

	    # First check to see if --with-tclooconfig was specified.
	    if test x"${with_tclooconfig}" != x ; then
		case ${with_tclooconfig} in
		    */tclooConfig.sh )
			if test -f ${with_tclooconfig}; then
			    AC_MSG_WARN([--with-tcloo argument should refer to directory containing tclooConfig.sh, not to tclooConfig.sh itself])
			    with_tclooconfig=`echo ${with_tclooconfig} | sed 's!/tclooConfig\.sh$!!'`
			fi ;;
		esac
		if test -f "${with_tclooconfig}/tclooConfig.sh" ; then
		    ac_cv_c_tclooconfig=`(cd ${with_tclooconfig}; pwd)`
		else
		    AC_MSG_ERROR([${with_tclooconfig} directory doesn't contain tclooConfig.sh])
		fi
	    fi

	    # then check for a private TclOO library
	    if test x"${ac_cv_c_tclooconfig}" = x ; then
		for i in \
			../oo \
			`ls -dr ../TclOO[[0-1]].[[0-9]].[[0-9]]* 2>/dev/null` \
			`ls -dr ../TclOO[[0-1]].[[0-9]] 2>/dev/null` \
			`ls -dr ../TclOO[[0-1]].[[0-9]]* 2>/dev/null` \
			../../oo \
			`ls -dr ../../TclOO[[0-1]].[[0-9]].[[0-9]]* 2>/dev/null` \
			`ls -dr ../../TclOO[[0-1]].[[0-9]] 2>/dev/null` \
			`ls -dr ../../TclOO[[0-1]].[[0-9]]* 2>/dev/null` \
			../../../oo \
			`ls -dr ../../../TclOO[[0-1]].[[0-9]].[[0-9]]* 2>/dev/null` \
			`ls -dr ../../../TclOO[[0-1]].[[0-9]] 2>/dev/null` \
			`ls -dr ../../../TclOO[[0-1]].[[0-9]]* 2>/dev/null` ; do
		    if test -f "$i/tclooConfig.sh" ; then
			ac_cv_c_tclooconfig=`(cd $i; pwd)`
			break
		    fi
		done
	    fi

	    # check in a few common install locations
	    if test x"${ac_cv_c_tclooconfig}" = x ; then
		for i in `ls -d ${libdir}/TclOO[[0-1]].* 2>/dev/null` \
			`ls -d ${exec_prefix}/lib/TclOO[[0-1]].* 2>/dev/null` \
			`ls -d ${prefix}/lib/TclOO[[0-1]].* 2>/dev/null` \
			`ls -d /usr/local/lib/TclOO[[0-1]].* 2>/dev/null` \
			`ls -d /usr/contrib/lib/TclOO[[0-1]].* 2>/dev/null` \
			`ls -d /usr/lib/TclOO[[0-1]].* 2>/dev/null` \
                        `ls -d ${libdir} 2>/dev/null` \
			`ls -d ${exec_prefix}/lib 2>/dev/null` \
			`ls -d ${prefix}/lib 2>/dev/null` \
			`ls -d /usr/local/lib 2>/dev/null` \
			`ls -d /usr/contrib/lib 2>/dev/null` \
			`ls -d /usr/lib 2>/dev/null` \
			; do
		    if test -f "$i/tclooConfig.sh" ; then
			ac_cv_c_tclooconfig=`(cd $i; pwd)`
			break
		    fi
		done
	    fi

	    # TEA specific: on Windows, check in common installation locations
	    if test "${TEA_PLATFORM}" = "windows" \
		-a x"${ac_cv_c_tclooconfig}" = x ; then
		for i in `ls -d C:/Tcl/lib/TclOO[[0-1]].* 2>/dev/null` \
			`ls -d C:/Progra~1/Tcl/lib/TclOO[[0-1]].* 2>/dev/null` \
                        `ls -d C:/Tcl/lib 2>/dev/null` \
			`ls -d C:/Progra~1/Tcl/lib 2>/dev/null` \
			; do
		    if test -f "$i/tclooConfig.sh" ; then
			ac_cv_c_tclooconfig=`(cd $i; pwd)`
			break
		    fi
		done
	    fi

	    # check in a few other private locations
	    if test x"${ac_cv_c_tclooconfig}" = x ; then
		for i in \
			${srcdir}/../tcloo \
			`ls -dr ${srcdir}/../TclOO[[0-1]].[[0-9]].[[0-9]]* 2>/dev/null` \
			`ls -dr ${srcdir}/../TclOO[[0-1]].[[0-9]] 2>/dev/null` \
			`ls -dr ${srcdir}/../TclOO[[0-1]].[[0-9]]* 2>/dev/null` ; do
		    if test -f "$i/tclooConfig.sh" ; then
			ac_cv_c_tclooconfig=`(cd $i; pwd)`
			break
		    fi
		done
	    fi
	])

	if test x"${ac_cv_c_tclooconfig}" = x ; then
	    TCLOO_BIN_DIR="# no TclOO configs found"
	    AC_MSG_WARN([Can't find TclOO configuration definitions])
	    exit 0
	else
	    no_tcloo=
	    TCLOO_BIN_DIR=${ac_cv_c_tclooconfig}
	    AC_MSG_RESULT([found ${TCLOO_BIN_DIR}/tclooConfig.sh])
	fi
    fi
])

dnl +------------------------------------------------------------------------
dnl | TEAX_LOAD_TCLOOCONFIG --
dnl |
dnl |	Load the tclooConfig.sh file
dnl |
dnl | Arguments:
dnl |	
dnl |	Requires the following vars to be set:
dnl |		TCLOO_BIN_DIR
dnl |
dnl | Results:
dnl |
dnl |	Sets the following vars that should be in tclooConfig.sh:
dnl | 		TCLOO_BIN_DIR
dnl +------------------------------------------------------------------------

AC_DEFUN([TEAX_LOAD_TCLOOCONFIG], [
    AC_MSG_CHECKING([for existence of ${TCLOO_BIN_DIR}/tclooConfig.sh])

    if test -f "${TCLOO_BIN_DIR}/tclooConfig.sh" ; then
        AC_MSG_RESULT([loading])
	. "${TCLOO_BIN_DIR}/tclooConfig.sh"
    else
        AC_MSG_RESULT([could not find ${TCLOO_BIN_DIR}/tclooConfig.sh])
    fi

    # eval is required to do the TCLOO_DBGX substitution
    eval "TCLOO_STUB_LIB_FILE=\"${TCLOO_STUB_LIB_FILE}\""

    # eval is required to do the TCLOO_DBGX substitution
    eval "TCLOO_LIB_SPEC=\"${TCLOO_LIB_SPEC}\""
    eval "TCLOO_STUB_LIB_FLAG=\"${TCLOO_STUB_LIB_FLAG}\""
    eval "TCLOO_STUB_LIB_SPEC=\"${TCLOO_STUB_LIB_SPEC}\""

    AC_SUBST(TCLOO_VERSION)
    AC_SUBST(TCLOO_BIN_DIR)
    AC_SUBST(TCLOO_LIB_SPEC)
    AC_SUBST(TCLOO_STUB_LIB_SPEC)
    AC_SUBST(TCLOO_INCLUDE_SPEC)
    AC_SUBST(TCLOO_PRIVATE_INCLUDE_SPEC)

    # TEA specific:
    AC_SUBST(TCLOO_CFLAGS)
])

dnl +------------------------------------------------------------------------
dnl | TEAX_PATH_TDBCCONFIG --
dnl |
dnl |	Locate the tdbcConfig.sh file
dnl |
dnl | Arguments:
dnl |	none
dnl |
dnl | Results:
dnl |
dnl |	Adds the following arguments to configure:
dnl |		--with-tdbc=...
dnl |
dnl |	Defines the following vars:
dnl |		TDBC_BIN_DIR	Full path to the directory containing
dnl |				the tdbcConfig.sh file
dnl +------------------------------------------------------------------------

AC_DEFUN([TEAX_PATH_TDBCCONFIG], [
    #
    # Ok, lets find the tdbc configuration
    # First, look for one uninstalled.
    # the alternative search directory is invoked by --with-tdbc
    #

    if test x"${no_tdbc}" = x ; then
	# we reset no_tdbc in case something fails here
	no_tdbc=true
	AC_ARG_WITH(tdbc,
	    AC_HELP_STRING([--with-tdbc],
		[directory containing Tdbc configuration (tdbcConfig.sh)]),
	    with_tdbcconfig=${withval})
	AC_MSG_CHECKING([for Tdbc configuration])
	AC_CACHE_VAL(ac_cv_c_tdbcconfig,[

	    # First check to see if --with-tdbcconfig was specified.
	    if test x"${with_tdbcconfig}" != x ; then
		case ${with_tdbcconfig} in
		    */tdbcConfig.sh )
			if test -f ${with_tdbcconfig}; then
			    AC_MSG_WARN([--with-tdbc argument should refer to directory containing tdbcConfig.sh, not to tdbcConfig.sh itself])
			    with_tdbcconfig=`echo ${with_tdbcconfig} | sed 's!/tdbcConfig\.sh$!!'`
			fi ;;
		esac
		if test -f "${with_tdbcconfig}/tdbcConfig.sh" ; then
		    ac_cv_c_tdbcconfig=`(cd ${with_tdbcconfig}; pwd)`
		else
		    AC_MSG_ERROR([${with_tdbcconfig} directory doesn't contain tdbcConfig.sh])
		fi
	    fi

	    # then check for a private Tdbc library
	    if test x"${ac_cv_c_tdbcconfig}" = x ; then
		for i in \
			../oo \
			`ls -dr ../Tdbc[[0-1]].[[0-9]].[[0-9]]* 2>/dev/null` \
			`ls -dr ../Tdbc[[0-1]].[[0-9]] 2>/dev/null` \
			`ls -dr ../Tdbc[[0-1]].[[0-9]]* 2>/dev/null` \
			../../oo \
			`ls -dr ../../Tdbc[[0-1]].[[0-9]].[[0-9]]* 2>/dev/null` \
			`ls -dr ../../Tdbc[[0-1]].[[0-9]] 2>/dev/null` \
			`ls -dr ../../Tdbc[[0-1]].[[0-9]]* 2>/dev/null` \
			../../../oo \
			`ls -dr ../../../Tdbc[[0-1]].[[0-9]].[[0-9]]* 2>/dev/null` \
			`ls -dr ../../../Tdbc[[0-1]].[[0-9]] 2>/dev/null` \
			`ls -dr ../../../Tdbc[[0-1]].[[0-9]]* 2>/dev/null` ; do
		    if test -f "$i/tdbcConfig.sh" ; then
			ac_cv_c_tdbcconfig=`(cd $i; pwd)`
			break
		    fi
		done
	    fi

	    # check in a few common install locations
	    if test x"${ac_cv_c_tdbcconfig}" = x ; then
		for i in `ls -d ${libdir}/Tdbc[[0-1]].* 2>/dev/null` \
			`ls -d ${exec_prefix}/lib/Tdbc[[0-1]].* 2>/dev/null` \
			`ls -d ${prefix}/lib/Tdbc[[0-1]].* 2>/dev/null` \
			`ls -d /usr/local/lib/Tdbc[[0-1]].* 2>/dev/null` \
			`ls -d /usr/contrib/lib/Tdbc[[0-1]].* 2>/dev/null` \
			`ls -d /usr/lib/Tdbc[[0-1]].* 2>/dev/null` \
                        `ls -d ${libdir} 2>/dev/null` \
			`ls -d ${exec_prefix}/lib 2>/dev/null` \
			`ls -d ${prefix}/lib 2>/dev/null` \
			`ls -d /usr/local/lib 2>/dev/null` \
			`ls -d /usr/contrib/lib 2>/dev/null` \
			`ls -d /usr/lib 2>/dev/null` \
			; do
		    if test -f "$i/tdbcConfig.sh" ; then
			ac_cv_c_tdbcconfig=`(cd $i; pwd)`
			break
		    fi
		done
	    fi

	    # TEA specific: on Windows, check in common installation locations
	    if test "${TEA_PLATFORM}" = "windows" \
		-a x"${ac_cv_c_tdbcconfig}" = x ; then
		for i in `ls -d C:/Tcl/lib/Tdbc[[0-1]].* 2>/dev/null` \
			`ls -d C:/Progra~1/Tcl/lib/Tdbc[[0-1]].* 2>/dev/null` \
                        `ls -d C:/Tcl/lib 2>/dev/null` \
			`ls -d C:/Progra~1/Tcl/lib 2>/dev/null` \
			; do
		    if test -f "$i/tdbcConfig.sh" ; then
			ac_cv_c_tdbcconfig=`(cd $i; pwd)`
			break
		    fi
		done
	    fi

	    # check in a few other private locations
	    if test x"${ac_cv_c_tdbcconfig}" = x ; then
		for i in \
			${srcdir}/../tdbc \
			`ls -dr ${srcdir}/../Tdbc[[0-1]].[[0-9]].[[0-9]]* 2>/dev/null` \
			`ls -dr ${srcdir}/../Tdbc[[0-1]].[[0-9]] 2>/dev/null` \
			`ls -dr ${srcdir}/../Tdbc[[0-1]].[[0-9]]* 2>/dev/null` ; do
		    if test -f "$i/tdbcConfig.sh" ; then
			ac_cv_c_tdbcconfig=`(cd $i; pwd)`
			break
		    fi
		done
	    fi
	])

	if test x"${ac_cv_c_tdbcconfig}" = x ; then
	    TDBC_BIN_DIR="# no Tdbc configs found"
	    AC_MSG_WARN([Can't find Tdbc configuration definitions])
	    exit 0
	else
	    no_tdbc=
	    TDBC_BIN_DIR=${ac_cv_c_tdbcconfig}
	    AC_MSG_RESULT([found ${TDBC_BIN_DIR}/tdbcConfig.sh])
	fi
    fi
])

dnl +------------------------------------------------------------------------
dnl | TEAX_LOAD_TDBCCONFIG --
dnl |
dnl |	Load the tdbcConfig.sh file
dnl |
dnl | Arguments:
dnl |	
dnl |	Requires the following vars to be set:
dnl |		TDBC_BIN_DIR
dnl |
dnl | Results:
dnl |
dnl |	Sets the following vars that should be in tdbcConfig.sh:
dnl | 		TDBC_BIN_DIR
dnl +------------------------------------------------------------------------

AC_DEFUN([TEAX_LOAD_TDBCCONFIG], [
    AC_MSG_CHECKING([for existence of ${TDBC_BIN_DIR}/tdbcConfig.sh])

    if test -f "${TDBC_BIN_DIR}/tdbcConfig.sh" ; then
        AC_MSG_RESULT([loading])
	. "${TDBC_BIN_DIR}/tdbcConfig.sh"
    else
        AC_MSG_RESULT([could not find ${TDBC_BIN_DIR}/tdbcConfig.sh])
    fi

    # eval is required to do the TDBC_DBGX substitution
    eval "TDBC_STUB_LIB_FILE=\"${TDBC_STUB_LIB_FILE}\""

    # eval is required to do the TDBC_DBGX substitution
    eval "TDBC_LIB_SPEC=\"${TDBC_LIB_SPEC}\""
    eval "TDBC_STUB_LIB_FLAG=\"${TDBC_STUB_LIB_FLAG}\""
    eval "TDBC_STUB_LIB_SPEC=\"${TDBC_STUB_LIB_SPEC}\""

    AC_SUBST(TDBC_VERSION)
    AC_SUBST(TDBC_BIN_DIR)
    AC_SUBST(TDBC_LIB_SPEC)
    AC_SUBST(TDBC_STUB_LIB_SPEC)
    AC_SUBST(TDBC_INCLUDE_SPEC)
    AC_SUBST(TDBC_PRIVATE_INCLUDE_SPEC)

    # TEA specific:
    AC_SUBST(TDBC_CFLAGS)
])

dnl Local Variables:
dnl mode: autoconf
dnl End:
