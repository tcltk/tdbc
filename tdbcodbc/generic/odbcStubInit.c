/*
 * odbcStubInit.c --
 *
 *	Stubs tables for the foreign ODBC libraries so that
 *	Tcl extensions can use them without the linker's knowing about them.
 *
 * @CREATED@ 2010-03-14 19:34:37Z by genExtStubs.tcl from ../generic/odbcStubDefs.txt
 *
 * Copyright (c) 2010 by Kevin B. Kenny.
 *
 * Please refer to the file, 'license.terms' for the conditions on
 * redistribution of this file and for a DISCLAIMER OF ALL WARRANTIES.
 *
 *-----------------------------------------------------------------------------
 */

#include <tcl.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "fakesql.h"

/*
 * Static data used in this file
 */

/*
 * Names of the libraries that might contain the ODBC API
 */

static const char* odbcStubLibNames[] = {
    /* @LIBNAMES@: DO NOT EDIT THESE NAMES */
    "odbc32", "odbc", NULL
    /* @END@ */
};
static const char* odbcOptLibNames[] = {
    "odbccp", "odbccp32", "odbcinst", NULL
};

/*
 * Names of the functions that we need from ODBC
 */

static const char* odbcSymbolNames[] = {
    /* @SYMNAMES@: DO NOT EDIT THESE NAMES */
    "SQLAllocHandle",
    "SQLBindParameter",
    "SQLCloseCursor",
    "SQLColumnsW",
    "SQLDataSourcesW",
    "SQLDescribeColW",
    "SQLDescribeParam",
    "SQLDisconnect",
    "SQLDriverConnectW",
    "SQLDriversW",
    "SQLEndTran",
    "SQLExecute",
    "SQLFetch",
    "SQLFreeHandle",
    "SQLGetConnectAttr",
    "SQLGetData",
    "SQLGetDiagFieldA",
    "SQLGetDiagRecW",
    "SQLGetTypeInfo",
    "SQLNumParams",
    "SQLNumResultCols",
    "SQLPrepareW",
    "SQLRowCount",
    "SQLSetConnectAttr",
    "SQLSetConnectOption",
    "SQLSetEnvAttr",
    "SQLTablesW",
    NULL
    /* @END@ */
};

/*
 * Table containing pointers to the functions named above.
 */

static odbcStubDefs odbcStubsTable;
odbcStubDefs* odbcStubs = &odbcStubsTable;

/*
 * Pointers to optional functions in ODBCINST
 */

BOOL INSTAPI (*SQLConfigDataSourceW)(HWND, WORD, LPCWSTR, LPCWSTR)
= NULL;
BOOL INSTAPI (*SQLConfigDataSource)(HWND, WORD, LPCSTR, LPCSTR)
= NULL;
BOOL INSTAPI (*SQLInstallerErrorW)(WORD, DWORD*, LPWSTR, WORD, WORD*)
= NULL;
BOOL INSTAPI (*SQLInstallerError)(WORD, DWORD*, LPSTR, WORD, WORD*)
= NULL;

/*
 *-----------------------------------------------------------------------------
 *
 * OdbcInitStubs --
 *
 *	Initialize the Stubs table for the ODBC API
 *
 * Results:
 *	Returns the handle to the loaded ODBC client library, or NULL
 *	if the load is unsuccessful. Leaves an error message in the
 *	interpreter.
 *
 *-----------------------------------------------------------------------------
 */

MODULE_SCOPE Tcl_LoadHandle
OdbcInitStubs(Tcl_Interp* interp,
				/* Tcl interpreter */
	      Tcl_LoadHandle* handle2Ptr)
				/* Pointer to a second load handle
				 * that represents the ODBCINST library */
{
    int i;
    int symc = sizeof(odbcSymbolNames)/sizeof(*odbcSymbolNames) - 1;
				/* TEMP - API to be revisited */
    int status;			/* Status of Tcl library calls */
    Tcl_Obj* path;		/* Path name of a module to be loaded */
    Tcl_Obj* shlibext;		/* Extension to use for load modules */
    Tcl_LoadHandle handle = NULL;
				/* Handle to a load module */

    SQLConfigDataSourceW = NULL;
    SQLConfigDataSource = NULL;
    SQLInstallerErrorW = NULL;
    SQLInstallerError = NULL;

    /*
     * Determine the shared library extension
     */
    status = Tcl_EvalEx(interp, "::info sharedlibextension", -1,
			TCL_EVAL_GLOBAL);
    if (status != TCL_OK) return NULL;
    shlibext = Tcl_GetObjResult(interp);
    Tcl_IncrRefCount(shlibext);

    /*
     * Walk the list of possible library names to find an ODBC client
     */
    status = TCL_ERROR;
    for (i = 0; status == TCL_ERROR && odbcStubLibNames[i] != NULL; ++i) {
	path = Tcl_NewStringObj(odbcStubLibNames[i], -1);
	Tcl_AppendObjToObj(path, shlibext);
	Tcl_IncrRefCount(path);
	Tcl_ResetResult(interp);

	/*
	 * Try to load a client library and resolve the ODBC API within it.
	 */
	status = Tcl_LoadFile(interp, path, symc, odbcSymbolNames,
			      odbcStubs, &handle);
	Tcl_DecrRefCount(path);
    }

    /*
     * If a client library is found, then try to load ODBCINST as well.
     */
    if (status == TCL_OK) {
	int status2 = TCL_ERROR;
	for (i = 0; status2 == TCL_ERROR && odbcOptLibNames[i] != NULL; ++i) {
	    path = Tcl_NewStringObj(odbcOptLibNames[i], -1);
	    Tcl_AppendObjToObj(path, shlibext);
	    Tcl_IncrRefCount(path);
	    status2 = Tcl_LoadFile(interp, path, 0, NULL, NULL, handle2Ptr);
	    if (status2 == TCL_OK) {
		SQLConfigDataSourceW = 
		    (BOOL INSTAPI (*)(HWND, WORD, LPCWSTR, LPCWSTR))
		    Tcl_FindSymbol(NULL, *handle2Ptr, "SQLConfigDataSourceW");
		if (SQLConfigDataSourceW == NULL) {
		    SQLConfigDataSource =
			(BOOL INSTAPI(*)(HWND, WORD, LPCSTR, LPCSTR))
			Tcl_FindSymbol(NULL, *handle2Ptr,
				       "SQLConfigDataSource");
		}
		SQLInstallerErrorW =
		    (BOOL INSTAPI(*)(WORD, DWORD*, LPWSTR, WORD, WORD*))
		    Tcl_FindSymbol(NULL, *handle2Ptr, "SQLInstallerErrorW");
		if (SQLInstallerErrorW == NULL) {
		    SQLInstallerError =
			(BOOL INSTAPI(*)(WORD, DWORD*, LPSTR, WORD, WORD*))
			Tcl_FindSymbol(NULL, *handle2Ptr, "SQLInstallerError");
		}
	    } else {
		Tcl_ResetResult(interp);
	    }
	}
    }

    /* 
     * Either we've successfully loaded a library (status == TCL_OK), 
     * or we've run out of library names (in which case status==TCL_ERROR
     * and the error message reflects the last unsuccessful load attempt).
     */
    Tcl_DecrRefCount(shlibext);
    if (status != TCL_OK) {
	return NULL;
    }
    return handle;
}
