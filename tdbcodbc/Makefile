# This is a rudimentary msys makefile for tdbc::odbc.  It will, of course,
# have to be replaced with a proper TEA-compliant one.  Assuming that I
# ever sort out how to make TEA work on Windows again!

RM = rm

INCLUDES = \
	-I/d/tcltest/include

DEFINES = \
	-D_WIN32 \
	-D_M_IX86 \
	-DNDEBUG \
	-DUSE_TCL_STUBS \
	-DUSE_TK_STUBS \
	-DUSE_TCLOO_STUBS \
	-DUSE_TDBC_STUBS \
	-DPACKAGE_NAME=\"tdbc::odbc\" \
	-DPACKAGE_VERSION=\"0.1a1\"

LIBS = \
	-L/d/tcltest/lib -ltclstub85 -ltkstub85 \
	-L/d/tcltest/lib/TclOO0.2a0 -lTclOOstub02a0 \
	-L/d/tcltest/lib/Tdbc0.1a1 -lTdbcstub01a1 \
	-lodbc32

# 	-L"/c/Program Files/Microsoft Platform SDK For Windows Server 2003 R2/lib" \

all: tdbcodbc01a1.dll

clean:
	$(RM) -f tdbcodbc01a1.dll tdbcodbc.o

tdbcodbc01a1.dll: tdbcodbc.o
	gcc -shared -Wall -g -o tdbcodbc01a1.dll tdbcodbc.o $(LIBS)

tdbcodbc.o: tdbcodbc.c
	gcc -Wall -g -c $(INCLUDES) $(DEFINES) tdbcodbc.c
