/*
 * tdbcodbc.c --
 *
 *	Bridge between TDBC (Tcl DataBase Connectivity) and ODBC.
 *
 * Copyright (c) 2008 by Kevin B. Kenny.
 *
 * Please refer to the file, 'license.terms' for the conditions on
 * redistribution of this file and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * $Id: $
 *
 *-----------------------------------------------------------------------------
 */

#include <tcl.h>

#if defined(BUILD_tdbcodbc)
#   define TDBCODBCAPI DLLEXPORT
#else
#   define TDBCODBCAPI DLLIMPORT
#endif

#include <tk.h>
#include <tclOO.h>
#include <tdbc.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <tkPlatDecls.h>
#include <sql.h>
#include <sqlucode.h>
#include <stdio.h>
#include <string.h>

#define USE_TK

TCL_DECLARE_MUTEX(hEnvMutex);	/* Mutex protecting the environment handle
				 * and its reference count */

static SQLHENV hEnv = SQL_NULL_HENV;
				/* Handle to the global ODBC environment */

static int hEnvRefCount = 0;	/* Reference count on the global environment */
#ifdef USE_TK
static int tkStubsInited = 0;	/* Flag == 1 if Tk stubs are initialized */
#endif

/*
 * Objects to create within the literal pool
 */

const char* LiteralValues[] = {
    "0",
    "1",
    "exists",
    "::info",
    "-encoding",
    "-isolation",
    "-readonly",
    "-timeout",
    "readuncommitted",
    "readcommitted",
    "repeatableread",
    "serializable",
    NULL
};
enum LiteralIndex {
    LIT_0,
    LIT_1,
    LIT_EXISTS,
    LIT_INFO,
    LIT_ENCODING,
    LIT_ISOLATION,
    LIT_READONLY,
    LIT_TIMEOUT,
    LIT_READUNCOMMITTED,
    LIT_READCOMMITTED,
    LIT_REPEATABLEREAD,
    LIT_SERIALIZABLE,
    LIT__END
};

/*
 * Structure that holds per-interpreter data for the ODBC package.
 */

typedef struct PerInterpData {
    int refCount;		/* Reference count */
    SQLHENV hEnv;		/* ODBC environment handle */
    Tcl_Obj* literals[LIT__END];
				/* Literal pool */
} PerInterpData;
#define IncrPerInterpRefCount(x)  \
    do {			  \
	++((x)->refCount);	  \
    } while(0)
#define DecrPerInterpRefCount(x)		\
    do {					\
	PerInterpData* _pidata = x;		\
	if ((--(_pidata->refCount)) <= 0) {	\
	    DeletePerInterpData(_pidata);	\
	}					\
    } while(0)

/* 
 * Structure that carries the data for an ODBC connection
 *
 * 	The ConnectionData structure is refcounted to simplify the
 *	destruction of statements associated with a connection.
 *	When a connection is destroyed, the subordinate namespace that
 *	contains its statements is taken down, destroying them. It's
 *	not safe to take down the ConnectionData until nothing is
 *	referring to it, which avoids taking down the hDBC until the
 *	other objects that refer to it vanish.
 */

typedef struct ConnectionData {
    int refCount;		/* Reference count. */
    PerInterpData* pidata;	/* Per-interpreter data */
    Tcl_Obj* connectionString;	/* Connection string actually used to
				 * connect to the database */
    SQLHDBC hDBC;		/* Connection handle */
    int flags;			/* Flags describing the state of the
				 * connection */
} ConnectionData;

/*
 * Flags for the state of an ODBC connection
 */

#define CONNECTION_FLAG_AUTOCOMMIT	(1<<0)
				/* Connection is in auto-commit mode */
#define CONNECTION_FLAG_XCN_ACTIVE	(1<<1)
				/* Connection has a transaction in progress.
				 * (Note that ODBC does not support nesting
				 * of transactions.) */
#define CONNECTION_FLAG_HAS_WVARCHAR	(1<<2)
				/* Connection supports WVARCHAR */

#define IncrConnectionRefCount(x) \
    do {			  \
	++((x)->refCount);	  \
    } while(0)
#define DecrConnectionRefCount(x)		\
    do {					\
	ConnectionData* conn = x;		\
	if ((--(conn->refCount)) <= 0) {	\
	    DeleteConnection(conn);		\
	}					\
    } while(0)

/*
 * Structure that carries the data for an ODBC prepared statement.
 *
 *	Just as with connections, statements need to defer taking down
 *	their client data until other objects (i.e., result sets) that
 * 	refer to them have had a chance to clean up. Hence, this
 *	structure is reference counted as well.
 */

typedef struct StatementData {
    int refCount;		/* Reference count */
    ConnectionData* cdata;	/* Data for the connection to which this
				 * statement pertains. */
    Tcl_Obj* subVars;	        /* List of variables to be substituted, in the
				 * order in which they appear in the 
				 * statement */
    SQLHSTMT hStmt;		/* Handle to the ODBC statement */
    SQLWCHAR* nativeSqlW;	/* SQL statement as wide chars */
    int nativeSqlLen;		/* Length of the statement */
    SQLWCHAR* nativeMatchPatternW;
				/* Match pattern for metadata queries */
    int nativeMatchPatLen;	/* Length of the match pattern */
    struct ParamData* params;	/* Pointer to an array of ParamData
				 * structures that describe the data types 
				 * of substituted parameters. */
    int typeNum;		/* Type number for a query of data types */
    Tcl_Obj* resultColNames;	/* Names of the columns in the result set */
    struct ParamData* results;	/* Pointer to the description of the
				 * result set columns */
    int flags;			/* Flags tracking the state of the
				 * StatementData */
} StatementData;
#define IncrStatementRefCount(x)		\
    do {					\
	++((x)->refCount);			\
    } while (0)
#define DecrStatementRefCount(x)		\
    do {					\
	StatementData* stmt = (x);		\
	if (--(stmt->refCount) <= 0) {		\
	    DeleteStatement(stmt);		\
	}					\
    } while(0)

/* Flags in StatementData */

#define STATEMENT_FLAG_HSTMT_BUSY 0x1
				/* This flag is set if hStmt is in use, in
				 * which case the progam must clone it if 
				 * another result set is needed */
#define STATEMENT_FLAG_RESULTS_KNOWN 0x2
				/* This flag is set if the result set
				 * has already been described. The result
				 * set metadata for a given statement is
				 * queried only once, and retained for
				 * use in future invocations. */
#define STATEMENT_FLAG_TABLES 0x4
				/* This flag is set if the statement is
				 * asking for table metadata */
#define STATEMENT_FLAG_COLUMNS 0x8
				/* This flag is set if the statement is
				 * asking for column metadata */
#define STATEMENT_FLAG_TYPES 0x10
				/* This flag is set if the statement is
				 * asking for data type metadata */

/*
 * Structure describing the data types of substituted parameters in
 * a SQL statement.
 */

typedef struct ParamData {
    int flags;			/* Flags regarding the parameters - see below */
    SQLSMALLINT dataType;	/* Data type */
    SQLULEN precision;		/* Size of the expected data */
    SQLSMALLINT scale;		/* Digits after decimal point of the
				 * expected data */
    SQLSMALLINT nullable;	/* Flag == 1 if the parameter is nullable */
} ParamData;

#define PARAM_KNOWN	1<<0	/* Something is known about the parameter */
#define PARAM_IN 	1<<1	/* Parameter is an input parameter */
#define PARAM_OUT 	1<<2	/* Parameter is an output parameter */
				/* (Both bits are set if parameter is
				 * an INOUT parameter) */

/*
 * Structure describing an ODBC result set.  The object that the Tcl
 * API terms a "result set" actually has to be represented by an ODBC
 * "statement", since an ODBC statement can have only one set of results
 * at any given time.
 */

typedef struct ResultSetData {
    int refCount;		/* Reference count */
    StatementData* sdata;	/* Statement that generated this result set */
    SQLHSTMT hStmt;		/* Handle to the ODBC statement object */
    SQLCHAR** bindStrings;	/* Buffers for binding string parameters */
    SQLLEN* bindStringLengths;	/* Lengths of the buffers */
    SQLLEN rowCount;		/* Number of rows affected by the statement */
} ResultSetData;
#define IncrResultSetRefCount(x)		\
    do {					\
	++((x)->refCount);			\
    } while (0)
#define DecrResultSetRefCount(x)		\
    do {					\
	ResultSetData* rs = (x);		\
	if (--(rs->refCount) <= 0) {		\
	    DeleteResultSet(rs);		\
	}					\
    } while(0)

/*
 * Structure for looking up a string that maps to an ODBC constant
 */

typedef struct OdbcConstant {
    const char* name;		/* Constant name */
    SQLSMALLINT value;		/* Constant value */
} OdbcConstant;

/*
 * Constants for the directions of parameter transmission
 */

const static OdbcConstant OdbcParamDirections[] = {
    { "in",		PARAM_KNOWN | PARAM_IN, },
    { "out",		PARAM_KNOWN | PARAM_OUT },
    { "inout",		PARAM_KNOWN | PARAM_IN | PARAM_OUT },
    { NULL,		0 }
};

/*
 * ODBC constants for the names of data types
 */

const static OdbcConstant OdbcTypeNames[] = {
    { "bigint",		SQL_BIGINT },
    { "binary",		SQL_BINARY },
    { "bit",		SQL_BIT } ,
    { "char",		SQL_CHAR } ,
    { "date",		SQL_DATE } ,
    { "decimal",	SQL_DECIMAL } ,
    { "double",		SQL_DOUBLE } ,
    { "float",		SQL_FLOAT } ,
    { "integer",	SQL_INTEGER } ,
    { "longvarbinary",	SQL_LONGVARBINARY } ,
    { "longvarchar",	SQL_LONGVARCHAR } ,
    { "numeric",	SQL_NUMERIC } ,
    { "real",		SQL_REAL } ,
    { "smallint",	SQL_SMALLINT } ,
    { "time",		SQL_TIME } ,
    { "timestamp",	SQL_TIMESTAMP } ,
    { "tinyint",	SQL_TINYINT } ,
    { "varbinary",	SQL_VARBINARY } ,
    { "varchar",	SQL_VARCHAR } ,
    { NULL,		-1 }
};

const static OdbcConstant OdbcIsolationLevels[] = {
    { "readuncommitted",	SQL_TXN_READ_UNCOMMITTED },
    { "readcommitted",		SQL_TXN_READ_COMMITTED },
    { "repeatableread",		SQL_TXN_REPEATABLE_READ },
    { "serializable",		SQL_TXN_SERIALIZABLE },
    { NULL,			0 }};
	    


/* Initialization script */

static const char initScript[] =
    "namespace eval ::tdbc::odbc {}\n"
    "tcl_findLibrary tdbc::odbc " PACKAGE_VERSION " " PACKAGE_VERSION
    " tdbcodbc.tcl TDBCODBC_LIBRARY ::tdbc::odbc::Library";

/* Prototypes for static functions appearing in this file */

static void DStringAppendWChars(Tcl_DString* ds, SQLWCHAR* ws, int len);
static SQLWCHAR* GetWCharStringFromObj(Tcl_Obj* obj, int* lengthPtr);

static void TransferSQLError(Tcl_Interp* interp, SQLSMALLINT handleType,
			     SQLHANDLE handle, const char* info);
static int SQLStateIs(SQLSMALLINT handleType, SQLHANDLE handle,
		      const char* sqlstate);
static int LookupOdbcConstant(Tcl_Interp* interp, const OdbcConstant* table,
			      const char* kind, Tcl_Obj* name,
			      SQLSMALLINT* valuePtr);
static int LookupOdbcType(Tcl_Interp* interp, Tcl_Obj* name,
			  SQLSMALLINT* valuePtr);
static Tcl_Obj* TranslateOdbcIsolationLevel(SQLINTEGER level,
					    Tcl_Obj* literals[]);
static SQLHENV GetHEnv(Tcl_Interp* interp);
static void DismissHEnv(void);
static SQLHSTMT AllocAndPrepareStatement(Tcl_Interp* interp,
					  StatementData* sdata);
static int GetResultSetDescription(Tcl_Interp* interp, StatementData* sdata,
				   SQLHSTMT hStmt);
static int ConfigureConnection(Tcl_Interp* interp, 
			       SQLHDBC hDBC,
			       PerInterpData* pidata,
			       int objc, Tcl_Obj *CONST objv[],
			       SQLUSMALLINT* connectFlagsPtr,
			       HWND* hParentWindowPtr);
static int ConnectionInitMethod(ClientData clientData, Tcl_Interp* interp,
				Tcl_ObjectContext context,
				int objc, Tcl_Obj *const objv[]);
static int ConnectionBeginTransactionMethod(ClientData clientData,
					    Tcl_Interp* interp,
					    Tcl_ObjectContext context,
					    int objc, Tcl_Obj *const objv[]);
static int ConnectionConfigureMethod(ClientData clientData,
				     Tcl_Interp* interp,
				     Tcl_ObjectContext context,
				     int objc, Tcl_Obj *const objv[]);
static int ConnectionEndXcnMethod(ClientData clientData,
				  Tcl_Interp* interp,
				  Tcl_ObjectContext context,
				  int objc, Tcl_Obj *const objv[]);
static int ConnectionHasWvarcharMethod(ClientData clientData,
				       Tcl_Interp* interp,
				       Tcl_ObjectContext context,
				       int objc, Tcl_Obj *const objv[]);
static int SetAutocommitFlag(Tcl_Interp* interp, ConnectionData* cdata,
			     SQLINTEGER flag);
static void DeleteCmd(ClientData clientData);
static int CloneCmd(Tcl_Interp* interp,
		    ClientData oldMetadata, ClientData* newMetadata);
static void DeleteConnectionMetadata(ClientData clientData);
static void DeleteConnection(ConnectionData* cdata);
static int CloneConnection(Tcl_Interp* interp, ClientData oldClientData,
			   ClientData* newClientData);
static StatementData* NewStatement(ConnectionData* cdata);
static int StatementInitMethod(ClientData clientData, Tcl_Interp* interp,
			       Tcl_ObjectContext context,
			       int objc, Tcl_Obj *const objv[]);
static int StatementParamListMethod(ClientData clientData, Tcl_Interp* interp,
				    Tcl_ObjectContext context,
				    int objc, Tcl_Obj *const objv[]);
static int StatementParamtypeMethod(ClientData clientData, Tcl_Interp* interp,
				    Tcl_ObjectContext context,
				    int objc, Tcl_Obj *const objv[]);
static int TablesStatementInitMethod(ClientData clientData, Tcl_Interp* interp,
				     Tcl_ObjectContext context,
				     int objc, Tcl_Obj *const objv[]);
static int ColumnsStatementInitMethod(ClientData clientData, Tcl_Interp* interp,
				      Tcl_ObjectContext context,
				      int objc, Tcl_Obj *const objv[]);
static int TypesStatementInitMethod(ClientData clientData, Tcl_Interp* interp,
				    Tcl_ObjectContext context,
				    int objc, Tcl_Obj *const objv[]);
static void DeleteStatementMetadata(ClientData clientData);
static void DeleteStatement(StatementData* sdata);
static int CloneStatement(Tcl_Interp* interp, ClientData oldClientData,
			  ClientData* newClientData);
static int ResultSetInitMethod(ClientData clientData, Tcl_Interp* interp,
			       Tcl_ObjectContext context,
			       int objc, Tcl_Obj *const objv[]);
static int ResultSetColumnsMethod(ClientData clientData, Tcl_Interp* interp,
				  Tcl_ObjectContext context,
				  int objc, Tcl_Obj *const objv[]);
static int ResultSetNextrowMethod(ClientData clientData, Tcl_Interp* interp,
				  Tcl_ObjectContext context,
				  int objc, Tcl_Obj *const objv[]);
static int GetCell(ResultSetData* rdata, Tcl_Interp* interp, 
		   int columnIndex, Tcl_Obj** retval);
static int ResultSetRowcountMethod(ClientData clientData, Tcl_Interp* interp,
				   Tcl_ObjectContext context,
				   int objc, Tcl_Obj *const objv[]);
static void DeleteResultSetMetadata(ClientData clientData);
static void DeleteResultSet(ResultSetData* rdata);
static int CloneResultSet(Tcl_Interp* interp, ClientData oldClientData,
			  ClientData* newClientData);
static void FreeBoundParameters(ResultSetData* rdata);
static void DeletePerInterpData(PerInterpData* pidata);
static int DatasourcesObjCmd(ClientData clientData, Tcl_Interp* interp,
			      int objc, Tcl_Obj *const objv[]);
static int DriversObjCmd(ClientData clientData, Tcl_Interp* interp,
			 int objc, Tcl_Obj *const objv[]);

/* Metadata type that holds connection data */

const static Tcl_ObjectMetadataType connectionDataType = {
    TCL_OO_METADATA_VERSION_CURRENT,
				/* version */
    "ConnectionData",		/* name */
    DeleteConnectionMetadata,	/* deleteProc */
    CloneConnection		/* cloneProc - should cause an error
				 * 'cuz connections aren't clonable */
};

/* Metadata type that holds statement data */

const static Tcl_ObjectMetadataType statementDataType = {
    TCL_OO_METADATA_VERSION_CURRENT,
				/* version */
    "StatementData",		/* name */
    DeleteStatementMetadata,	/* deleteProc */
    CloneStatement		/* cloneProc - should cause an error
				 * 'cuz statements aren't clonable */
};

/* Metadata type for result set data */

const static Tcl_ObjectMetadataType resultSetDataType = {
    TCL_OO_METADATA_VERSION_CURRENT,
				/* version */
    "ResultSetData",		/* name */
    DeleteResultSetMetadata,	/* deleteProc */
    CloneResultSet		/* cloneProc - should cause an error
				 * 'cuz result sets aren't clonable */
};

/* Method types of the connection methods that are implemented in C */

const static Tcl_MethodType ConnectionInitMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "init",			/* name */
    ConnectionInitMethod,	/* callProc */
    DeleteCmd,			/* deleteProc */
    CloneCmd			/* cloneProc */
};
const static Tcl_MethodType ConnectionBeginTransactionMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "begintransaction",		/* name */
    ConnectionBeginTransactionMethod,
				/* callProc */
    DeleteCmd,			/* deleteProc */
    CloneCmd			/* cloneProc */
};
const static Tcl_MethodType ConnectionConfigureMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "configure",		/* name */
    ConnectionConfigureMethod,	/* callProc */
    DeleteCmd,			/* deleteProc */
    CloneCmd			/* cloneProc */
};
const static Tcl_MethodType ConnectionEndXcnMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "endtransaction",		/* name */
    ConnectionEndXcnMethod,	/* callProc */
    DeleteCmd,			/* deleteProc */
    CloneCmd			/* cloneProc */
};
const static Tcl_MethodType ConnectionHasWvarcharMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "HasWvarchar",		/* name */
    ConnectionHasWvarcharMethod,
				/* callProc */
    DeleteCmd,			/* deleteProc */
    CloneCmd			/* cloneProc */
};

/* 
 * Methods to create on the connection class. Note that 'init', 'commit' and
 * 'rollback' are all special because they have non-NULL clientData.
 */

const static Tcl_MethodType* ConnectionMethods[] = {
    &ConnectionBeginTransactionMethodType,
    &ConnectionConfigureMethodType,
    &ConnectionHasWvarcharMethodType,
    NULL
};

/* Method types of the statement methods that are implemented in C */

const static Tcl_MethodType StatementInitMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "init",			/* name */
    StatementInitMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};
const static Tcl_MethodType StatementParamListMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "ParamList",		/* name */
    StatementParamListMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};
const static Tcl_MethodType StatementParamtypeMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "paramtype",		/* name */
    StatementParamtypeMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};

/* 
 * Methods to create on the statement class. 
 */

const static Tcl_MethodType* StatementMethods[] = {
    &StatementInitMethodType,
    &StatementParamListMethodType,
    &StatementParamtypeMethodType,
    NULL
};

/*
 * Method types for the class that implements the fake 'statement'
 * used to query the names and attributes of database tables.
 */

const static Tcl_MethodType TablesStatementInitMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "init",			/* name */
    TablesStatementInitMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};

/* Table of methods to instantiate on the 'tablesStatement' class */

const static Tcl_MethodType* TablesStatementMethods[] = {
    &TablesStatementInitMethodType,
    NULL
};

/*
 * Method types for the class that implements the fake 'statement'
 * used to query the names and attributes of database columns.
 */

const static Tcl_MethodType ColumnsStatementInitMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "init",			/* name */
    ColumnsStatementInitMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};

/* Table of methods to instantiate on the 'columnsStatement' class */

const static Tcl_MethodType* ColumnsStatementMethods[] = {
    &ColumnsStatementInitMethodType,
    NULL
};

/*
 * Method types for the class that implements the fake 'statement'
 * used to query the names and attributes of database types.
 */

const static Tcl_MethodType TypesStatementInitMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "init",			/* name */
    &TypesStatementInitMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};

/* Table of methods to instantiate on the 'typesStatement' class */

const static Tcl_MethodType* TypesStatementMethods[] = {
    &TypesStatementInitMethodType,
    NULL
};

/* Method types of the result set methods that are implemented in C */

const static Tcl_MethodType ResultSetInitMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "init",			/* name */
    ResultSetInitMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};
const static Tcl_MethodType ResultSetColumnsMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */    "columns",			/* name */
    ResultSetColumnsMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};
const static Tcl_MethodType ResultSetNextrowMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "nextrow",			/* name */
    ResultSetNextrowMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};
const static Tcl_MethodType ResultSetRowcountMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "rowcount",			/* name */
    ResultSetRowcountMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};


const static Tcl_MethodType* ResultSetMethods[] = {
    &ResultSetInitMethodType,
    &ResultSetColumnsMethodType,
    &ResultSetRowcountMethodType,
    NULL
};

/*
 *-----------------------------------------------------------------------------
 *
 * DStringAppendWChars --
 *
 *	Converts a wide-character string returned from ODBC into UTF-8
 *	and appends the result to a Tcl_DString.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Appends the given SQLWCHAR string to the given Tcl_DString, which
 *	must have been previously initialized.
 *
 *-----------------------------------------------------------------------------
 */

static void
DStringAppendWChars(
    Tcl_DString* ds,		/* Output string */
    SQLWCHAR* ws,		/* Input string */
    int len			/* Length of the input string in characters */
) {
    int i;
    char buf[TCL_UTF_MAX];
    for (i = 0; i < len; ++i) {
	int bytes = Tcl_UniCharToUtf((int) ws[i], buf);
	Tcl_DStringAppend(ds, buf, bytes);
    }
}

/*
 *-----------------------------------------------------------------------------
 *
 * GetWCharStringFromObj --
 *
 *	Get a string of SQLWCHAR from the string value of a Tcl object.
 *
 * Results:
 *	Returns a pointer to the string, which the caller is responsible
 *	for freeing.
 *
 * Side effects:
 *	Stores the length of the string in '*lengthPtr' if 'lengthPtr' 
 *	is not NULL
 *
 *-----------------------------------------------------------------------------
 */

static SQLWCHAR*
GetWCharStringFromObj(
    Tcl_Obj* obj,		/* Tcl object whose string rep is desired */
    int* lengthPtr		/* Length of the string */
) {
    int len = Tcl_GetCharLength(obj);
				/* Length of the input string in characters */
    SQLWCHAR* retval = (SQLWCHAR*) ckalloc((len + 1) * sizeof(SQLWCHAR));
				/* Buffer to hold the converted string */
    char* bytes = Tcl_GetStringFromObj(obj, NULL);
				/* UTF-8 representation of the input string */
    int i;
    Tcl_UniChar ch;

    for (i = 0; i < len; ++i) {
	bytes += Tcl_UtfToUniChar(bytes, &ch);
	retval[i] = ch;
    }
    retval[i] = 0;
    if (lengthPtr != NULL) {
	*lengthPtr = len;
    }
    return retval;
}

/*
 *-----------------------------------------------------------------------------
 *
 * TransferSQLError --
 *
 *	Transfers an error message and associated error code from ODBC
 *	to Tcl.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The interpreter's result is set to a formatted error message, and
 *	the error code is set to a three-element list: TDBC ODBC xxxxx,
 *	where xxxxx is the SQL state code.
 *
 *-----------------------------------------------------------------------------
 */

static void
TransferSQLError(
    Tcl_Interp* interp,		/* Tcl interpreter */
    SQLSMALLINT handleType,	/* Type of the handle for which the error
				 * has been reported. */
    SQLHANDLE handle,		/* Handle that reported the error */
    const char* info		/* Additional information to report */
) {
    SQLWCHAR state[6];		/* SQL state code */
    SQLINTEGER nativeError;	/* Native error code */
    SQLSMALLINT msgLen;		/* Length of the error message */
    SQLWCHAR msg[SQL_MAX_MESSAGE_LENGTH];
				/* Buffer to hold the error message */
    SQLSMALLINT i;		/* Loop index for going through diagnostics */
    const char* sep = "";	/* Separator string for messages */
    Tcl_Obj* resultObj;		/* Result string containing error message */
    Tcl_Obj* codeObj;		/* Error code object */
    Tcl_Obj* codes[2];		/* Temp storage to initialize codeObj */
    Tcl_Obj* lineObj;		/* Object holding one diagnostic */
    Tcl_DString bufferDS;	/* Buffer for transferring messages */

    resultObj = Tcl_NewObj();
    codes[0] = Tcl_NewStringObj("TDBC", 4);
    codes[1] = Tcl_NewStringObj("ODBC", 4);
    codeObj = Tcl_NewListObj(2, codes);

    /* Loop through the diagnostics */

    i = 1;
    while (SQLGetDiagRecW(handleType, handle, i, state, &nativeError,
			  msg, SQL_MAX_MESSAGE_LENGTH, &msgLen)
	   != SQL_NO_DATA) {

	/* Add the diagnostic to ::errorCode */

	Tcl_DStringInit(&bufferDS);
	DStringAppendWChars(&bufferDS, state, 5);
	lineObj = Tcl_NewStringObj(Tcl_DStringValue(&bufferDS),
				   Tcl_DStringLength(&bufferDS));
	Tcl_DStringFree(&bufferDS);
	Tcl_ListObjAppendElement(NULL, codeObj, lineObj);
	Tcl_ListObjAppendElement(NULL, codeObj, Tcl_NewIntObj(nativeError));

	/* Add the error message to the return value */

	Tcl_DStringInit(&bufferDS);
	DStringAppendWChars(&bufferDS, msg, msgLen);
	Tcl_AppendToObj(resultObj, sep, -1);
	Tcl_AppendToObj(resultObj, Tcl_DStringValue(&bufferDS),
			Tcl_DStringLength(&bufferDS));
	Tcl_DStringFree(&bufferDS);
	sep = "\n";
	++i;
    }					       
    if (info != NULL) {
	Tcl_AppendToObj(resultObj, "\n", -1);
	Tcl_AppendToObj(resultObj, info, -1);
    }

    /* Stash the information into the interpreter */

    Tcl_SetObjResult(interp, resultObj);
    Tcl_SetObjErrorCode(interp, codeObj);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SQLStateIs --
 *
 *	Determines whther SQLSTATE in the set of diagnostic records
 *	contains a particular state.
 *
 * Results:
 *	Returns 1 if the state matches, and 0 otherwise.
 *
 * This function is used primarily to look for the state "HYC00"
 * (Optional Function Not Implemented), but may also be used for
 * other states such as "HYT00" (Timeout Expired), "HY008"
 * (Operation Cancelled), "01004" (Data Truncated) and "01S02"
 * (Option Value Changed). 
 *
 *-----------------------------------------------------------------------------
 */

static int
SQLStateIs(
    SQLSMALLINT handleType, 	/* Type of handle reporting the state */
    SQLHANDLE handle,		/* Handle that reported the state */
    const char* sqlstate	/* State to look for */
) {
    SQLCHAR state[6];		/* SQL state code from the diagnostic record */
    SQLSMALLINT stateLen;	/* String length of the state code */
    SQLSMALLINT i;		/* Loop index */

    i = 1;
    while (SQLGetDiagFieldA(handleType, handle, i, SQL_DIAG_SQLSTATE,
			    (SQLPOINTER) state, sizeof (state), &stateLen)
	   != SQL_NO_DATA) {
	if (stateLen >= 0 && !strcmp(sqlstate, (const char*) state)) {
	    return 1;
	}
    }
    return 0;
}

/*
 *-----------------------------------------------------------------------------
 *
 * LookupOdbcConstant --
 *
 *	Looks up an ODBC enumerated constant in a table.
 *
 * Results:
 *	Returns a standard Tcl result, with an error message stored in
 *	the result of the provided Tcl_Interp if it is not NULL.
 *
 * Side effects:
 *	If successful, stores the enumerated value in '*valuePtr'
 *
 *-----------------------------------------------------------------------------
 */

static int
LookupOdbcConstant(
    Tcl_Interp* interp,		/* Tcl interpreter */
    const OdbcConstant* table,	/* Table giving the enumerations */
    const char* kind,		/* String descibing the kind of enumerated
				 * object being looked up */
    Tcl_Obj* name,		/* Name being looked up */
    SQLSMALLINT* valuePtr	/* Pointer to the returned value */
) {
    int index;
    if (Tcl_GetIndexFromObjStruct(interp, name, (void*)table,
				  sizeof(OdbcConstant), kind, TCL_EXACT,
				  &index) != TCL_OK) {
	return TCL_ERROR;
    }
    *valuePtr = (SQLSMALLINT) table[index].value;
    return TCL_OK;
}

static inline int LookupOdbcType(
    Tcl_Interp* interp, 
    Tcl_Obj* name,
    SQLSMALLINT* valuePtr
) {
    return LookupOdbcConstant(interp, OdbcTypeNames, "SQL data type",
			      name, valuePtr);
}

/*
 *-----------------------------------------------------------------------------
 *
 * TranslateOdbcIsolationLevel --
 *
 *	Translates an ODBC isolation level into human-readable form.
 *
 * Results:
 *	Returns a Tcl_Obj with the human-readable level.
 *
 *-----------------------------------------------------------------------------
 */

static Tcl_Obj*
TranslateOdbcIsolationLevel(
    SQLINTEGER level, 		/* Isolation level */
    Tcl_Obj* literals[]		/* Pointer to the literal pool */
) {
    if (level & SQL_TXN_SERIALIZABLE) {
	return literals[LIT_SERIALIZABLE];
    }
    if (level & SQL_TXN_REPEATABLE_READ) {
	return literals[LIT_REPEATABLEREAD];
    }
    if (level & SQL_TXN_READ_COMMITTED) {
	return literals[LIT_READCOMMITTED];
    }
    return literals[LIT_READUNCOMMITTED];
}

/*
 *-----------------------------------------------------------------------------
 *
 * GetHEnv --
 *
 *	Retrieves the global environment handle for ODBC.
 *
 * Results:
 *	Returns the global environment handle. If the allocation of the
 *	global enviroment fails, returns SQL_NULL_ENV. If 'interp' is
 *	not NULL, stores an error message in the interpreter.
 *
 * Maintains a reference count so that the handle is closed when the
 * last use of ODBC in the process goes away.
 *
 *-----------------------------------------------------------------------------
 */

static SQLHENV
GetHEnv(
    Tcl_Interp* interp		/* Interpreter for error reporting, or NULL */
) {
    Tcl_MutexLock(&hEnvMutex);
    if (hEnvRefCount == 0) {
	/*
	 * This is the first reference to ODBC in this process.
	 * Call the ODBC library to allocate the environment.
	 */
	RETCODE rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv);
	if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
	    rc = SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION,
			       (SQLPOINTER) SQL_OV_ODBC3, 0);
	}
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	    /*
	     * The call failed. Report the error.
	     */
	    if (hEnv != SQL_NULL_HENV) {
		if (interp != NULL) {
		    TransferSQLError(interp, SQL_HANDLE_ENV, hEnv,
				     "(allocating environment handle)");
		}
		SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
		hEnv = SQL_NULL_HENV;
	    } else {
		Tcl_SetObjResult(interp,
				 Tcl_NewStringObj("Could not allocate the "
						  "ODBC SQL environment.", -1));
		Tcl_SetErrorCode(interp, "TDBC", "ODBC", "HY001", "-1", NULL);
	    }
	}
    }
    if (hEnv != SQL_NULL_HENV) {
	++hEnvRefCount;
    }
    Tcl_MutexUnlock(&hEnvMutex);
    return hEnv;
}

/*
 *-----------------------------------------------------------------------------
 *
 * DismissHEnv --
 *
 *	Notifies that the SQLHENV returned from GetHEnv is no longer
 *	in use.
 *
 * Side effects:
 *	Decreases the refcount of the handle, and returns it if all
 *	extant refs have gone away.
 *
 *-----------------------------------------------------------------------------
 */

static void
DismissHEnv(void)
{
    Tcl_MutexLock(&hEnvMutex);
    if (--hEnvRefCount == 0) {
	SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
	hEnv = SQL_NULL_HANDLE;
    }
    Tcl_MutexUnlock(&hEnvMutex);
}

/*
 *-----------------------------------------------------------------------------
 *
 * AllocAndPrepareStatement --
 *
 *	Allocates an ODBC statement handle, and prepares SQL code in it.
 *
 * Results:
 *	Returns the handle, or SQL_NULL_HSTMT if an error occurs.
 *
 *-----------------------------------------------------------------------------
 */

static SQLHSTMT
AllocAndPrepareStatement(
    Tcl_Interp* interp,		/* Tcl interpreter for error reporting */
    StatementData* sdata	/* Data describing the statement */
) {
    SQLRETURN rc;
    SQLHSTMT hStmt;
    ConnectionData* cdata = sdata->cdata;
    if (sdata->flags & (STATEMENT_FLAG_TABLES 
			| STATEMENT_FLAG_COLUMNS
			| STATEMENT_FLAG_TYPES)) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj("cannot have multiple result "
						  "sets in this context", -1));
	return SQL_NULL_HSTMT;
    }
    rc = SQLAllocHandle(SQL_HANDLE_STMT, cdata->hDBC, &hStmt);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	TransferSQLError(interp, SQL_HANDLE_DBC, cdata->hDBC, 
			 "(allocating statement handle)");
	return SQL_NULL_HSTMT;
    }
    rc = SQLPrepareW(hStmt, sdata->nativeSqlW, sdata->nativeSqlLen);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	TransferSQLError(interp, SQL_HANDLE_STMT, hStmt,
			 "(preparing statement)");
	SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
	return SQL_NULL_HSTMT;
    }
    return hStmt;
}

/*
 *-----------------------------------------------------------------------------
 *
 * GetResultSetDescription --
 *
 *	Describes the result set of an ODBC statement
 *
 * Results:
 *	Returns a standard Tcl result and stores an error message in the
 *	interpreter result if a failure occurs.
 *
 * Side effects:
 *	Stores column names and type information in 'sdata' and 
 *	updates the flags to indicate that the data are present.
 *
 *-----------------------------------------------------------------------------
 */

static int
GetResultSetDescription(
    Tcl_Interp* interp,		/* Tcl interpreter */
    StatementData* sdata,	/* Statement data object */
    SQLHSTMT hStmt		/* Handle to the ODBC statement */
) {
    SQLRETURN rc;		/* Return code from ODBC operations */
    Tcl_Obj* colNames;		/* List of the column names */
    SQLSMALLINT nColumns;	/* Number of result set columns */
    SQLWCHAR colNameBuf[40];	/* Buffer to hold the column name */
    SQLSMALLINT colNameLen = 40; 
				/* Length of the column name */
    SQLSMALLINT colNameAllocLen = 40;
				/* Allocated length of the column name */
    SQLWCHAR* colNameW = colNameBuf;
				/* Name of the current column */
    Tcl_DString colNameDS;	/* Name of the current column, translated */
    Tcl_Obj* colNameObj;	/* Name of the current column, packaged in
				 * a Tcl_Obj */
    SQLSMALLINT i;
    int retry;
    int status = TCL_ERROR;

    /* Count the columns of the result set */

    rc = SQLNumResultCols(hStmt, &nColumns);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	TransferSQLError(interp, SQL_HANDLE_STMT, hStmt, 
			 "(getting number of result columns)");
	return TCL_ERROR;
    }
    colNames = Tcl_NewObj();
    Tcl_IncrRefCount(colNames);
    if (nColumns != 0) {

	/* 
	 * If there are columns in the result set, find their names and
	 * data types.
	 */

	sdata->results = (ParamData*) ckalloc(nColumns * sizeof(ParamData));
	for (i = 0; i < nColumns; ++i) {
	    retry = 0;
	    do {

		/* Describe one column of the result set */

		rc = SQLDescribeColW(hStmt, i + 1, colNameW, 
				     colNameAllocLen, &colNameLen,
				     &(sdata->results[i].dataType),
				     &(sdata->results[i].precision),
				     &(sdata->results[i].scale),
				     &(sdata->results[i].nullable));

		/* 
		 * Reallocate the name buffer and retry if the buffer was
		 * too small.
		 */		 

		if (colNameLen < colNameAllocLen) {
		    retry = 0;
		} else {
		    colNameAllocLen = 2 * colNameLen + 1;
		    if (colNameW != colNameBuf) {
			ckfree((char*) colNameW);
		    }
		    colNameW = (SQLWCHAR*)
			ckalloc(colNameAllocLen * sizeof(SQLWCHAR));
		    retry = 1;
		}
	    } while (retry);

	    /* Bail out on an ODBC error */

	    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		char info[80];
		sprintf(info, "(describing result column #%d)", i+1);
		TransferSQLError(interp, SQL_HANDLE_STMT, hStmt, info);
		Tcl_DecrRefCount(colNames);
		ckfree((char*)sdata->results);
		goto cleanup;
	    }
	    
	    /* Add column name to the list of column names */

	    Tcl_DStringInit(&colNameDS);
	    DStringAppendWChars(&colNameDS, colNameW, colNameLen);
	    colNameObj = Tcl_NewStringObj(Tcl_DStringValue(&colNameDS),
					  Tcl_DStringLength(&colNameDS));
	    Tcl_ListObjAppendElement(NULL, colNames, colNameObj);
	    Tcl_DStringFree(&colNameDS);
	}
    }

    /* Success: store the list of column names */

    sdata->resultColNames = colNames;
    sdata->flags |= STATEMENT_FLAG_RESULTS_KNOWN;
    status = TCL_OK;

    /* Clean up the column name buffer if we reallocated it. */

 cleanup:
    if (colNameW != colNameBuf) {
	ckfree((char*) colNameW);
    }
    return status;
    
}

/*
 *-----------------------------------------------------------------------------
 *
 * ConfigureConnection --
 *
 *	Processes configuration options for an ODBC connection.
 *
 * Results:
 *	Returns a standard Tcl result; if TCL_ERROR is returned, the
 *	interpreter result is set to an error message.
 *
 * Side effects:
 *	Makes appropriate SQLSetConnectAttr calls to set the connection
 *	attributes.  If connectFlagsPtr or hMainWindowPtr are not NULL,
 *	also accepts a '-parent' option, sets *connectFlagsPtr to
 *	SQL_DRIVER_COMPLETE_REQUIED or SQL_DRIVER_NOPROMPT according
 *	to whether '-parent' is supplied, and *hParentWindowPtr to the
 *	HWND corresponding to the parent window.
 *
 * objc,objv are presumed to frame just the options, with positional
 * parameters already stripped. The following options are accepted:
 *
 * -parent PATH
 *	Specifies the path name of a parent window to use in a connection
 *	dialog.
 *
 *-----------------------------------------------------------------------------
 */

static int
ConfigureConnection(
    Tcl_Interp* interp,		/* Tcl interpreter */
    SQLHDBC hDBC,		/* Handle to the connection */
    PerInterpData* pidata,	/* Package-global data */
    int objc,			/* Option count */
    Tcl_Obj *CONST objv[],	/* Option vector */
    SQLUSMALLINT* connectFlagsPtr,
				/* Pointer to the driver connection options */
    HWND* hParentWindowPtr	/* Handle to the parent window for a 
				 * connection dialog */
) {

    /* Configuration options */

    const static char* options[] = {
	"-encoding",
	"-isolation",
	"-parent",
	"-readonly",
	"-timeout",
	NULL
    };
    enum optionType {
	COPTION_ENCODING,
	COPTION_ISOLATION,
	COPTION_PARENT,
	COPTION_READONLY,
	COPTION_TIMEOUT
    };

    int indx;			/* Index of the current option */
#ifdef USE_TK
    Tk_Window tkwin;		/* Window identifier of the parent of the
				 * connection dialog */
#endif
    Tcl_Obj** literals = pidata->literals;
				/* Literal pool */
    Tcl_Obj* retval;		/* Return value from this command */
    Tcl_Encoding sysEncoding;	/* The system encoding */
    Tcl_Encoding newEncoding;	/* The requested encoding */
    const char* encName;	/* The name of the system encoding */
    int i;
    int j;
    SQLINTEGER mode;		/* Access mode of the database */
    SQLSMALLINT isol;		/* Isolation level */
    SQLINTEGER seconds;		/* Timeout value in seconds */
    SQLRETURN rc;		/* Return code from SQL operations */

    if (connectFlagsPtr) {
	*connectFlagsPtr = SQL_DRIVER_NOPROMPT;
    }
    if (hParentWindowPtr) {
	*hParentWindowPtr = NULL;
    }

    if (objc == 0) {

	/* return configuration options */

	retval = Tcl_NewObj();

	/* -encoding -- The ODBC encoding should be the system encoding */

	sysEncoding = Tcl_GetEncoding(interp, NULL);
	if (sysEncoding == NULL) {
	    encName = "iso8859-1";
	} else {
	    encName = Tcl_GetEncodingName(sysEncoding);
	}
	Tcl_ListObjAppendElement(NULL, retval, literals[LIT_ENCODING]);
	Tcl_ListObjAppendElement(NULL, retval, Tcl_NewStringObj(encName, -1));
	if (sysEncoding != NULL) {
	    Tcl_FreeEncoding(sysEncoding);
	}

	/* -isolation */

	rc = SQLGetConnectAttr(hDBC, SQL_ATTR_TXN_ISOLATION,
			       (SQLPOINTER) &mode, 0, NULL);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	    TransferSQLError(interp, SQL_HANDLE_DBC, hDBC, 
			     "(getting isolation level of connection)");
	    return TCL_ERROR;
	}
	Tcl_ListObjAppendElement(NULL, retval, literals[LIT_ISOLATION]);
	Tcl_ListObjAppendElement(NULL, retval,
				 TranslateOdbcIsolationLevel(mode, literals));

	/* -readonly */

	rc = SQLGetConnectAttr(hDBC, SQL_ATTR_ACCESS_MODE,
			       (SQLPOINTER) &mode, 0, NULL);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	    TransferSQLError(interp, SQL_HANDLE_DBC, hDBC, 
			     "(getting access mode of connection)");
	    return TCL_ERROR;
	}
	Tcl_ListObjAppendElement(NULL, retval, literals[LIT_READONLY]);
	Tcl_ListObjAppendElement(NULL, retval, 
				 Tcl_NewIntObj(mode == SQL_MODE_READ_ONLY));

	/* -timeout */

	rc = SQLGetConnectAttr(hDBC, SQL_ATTR_CONNECTION_TIMEOUT,
			       (SQLPOINTER)&seconds, 0, NULL);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	    if (SQLStateIs(SQL_HANDLE_DBC, hDBC, "HYC00")) {
		seconds = 0;
	    } else {
		TransferSQLError(interp, SQL_HANDLE_DBC, hDBC, 
				 "(getting connection timeout value)");
		return TCL_ERROR;
	    }
	}
	Tcl_ListObjAppendElement(NULL, retval, literals[LIT_TIMEOUT]);
	Tcl_ListObjAppendElement(NULL, retval,
				 Tcl_NewIntObj(1000 * (int) seconds));

	/* end of options */

	Tcl_SetObjResult(interp, retval);
	return TCL_OK;

    } else if (objc == 1) {

	/* look up a single configuration option */

	if (Tcl_GetIndexFromObj(interp, objv[0], options, "option",
				0, &indx) != TCL_OK) {
	    return TCL_ERROR;
	}

	switch (indx) {

	case COPTION_ENCODING:
	    sysEncoding = Tcl_GetEncoding(interp, NULL);
	    if (sysEncoding == NULL) {
		encName = "iso8859-1";
	    } else {
		encName = Tcl_GetEncodingName(sysEncoding);
	    }
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(encName, -1));
	    if (sysEncoding != NULL) {
		Tcl_FreeEncoding(sysEncoding);
	    }
	    break;

	case COPTION_ISOLATION:
	    rc = SQLGetConnectAttr(hDBC, SQL_ATTR_TXN_ISOLATION,
				   (SQLPOINTER) &mode, 0, NULL);
	    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		TransferSQLError(interp, SQL_HANDLE_DBC, hDBC, 
				 "(getting isolation level of connection)");
		return TCL_ERROR;
	    }
	    Tcl_SetObjResult(interp,
			     TranslateOdbcIsolationLevel(mode, literals));
	    break;

	case COPTION_PARENT:
	    Tcl_SetObjResult(interp,
			     Tcl_NewStringObj("-parent option cannot "
					      "be used after connection "
					      "is established", -1));
	    Tcl_SetErrorCode(interp, "TDBC", "ODBC", "HY010", "-1", NULL);
	    return TCL_ERROR;

	case COPTION_READONLY:
	    rc = SQLGetConnectAttr(hDBC, SQL_ATTR_ACCESS_MODE,
				   (SQLPOINTER) &mode, 0, NULL);
	    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		TransferSQLError(interp, SQL_HANDLE_DBC, hDBC, 
				 "(getting access mode of connection)");
		return TCL_ERROR;
	    }
	    Tcl_SetObjResult(interp,
			     Tcl_NewIntObj(mode == SQL_MODE_READ_ONLY));
	    break;

	case COPTION_TIMEOUT:
	    rc = SQLGetConnectAttr(hDBC, SQL_ATTR_CONNECTION_TIMEOUT,
				   (SQLPOINTER)&seconds, 0, NULL);
	    if (SQLStateIs(SQL_HANDLE_DBC, hDBC, "HYC00")) {
		seconds = 0;
	    } else {
		if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		    TransferSQLError(interp, SQL_HANDLE_DBC, hDBC, 
				     "(getting connection timeout value)");
		    return TCL_ERROR;
		}
	    }
	    Tcl_SetObjResult(interp, Tcl_NewIntObj(1000 * (int) seconds));
	    break;

	}

	return TCL_OK;

    }

    /* set configuration options */

    for (i = 0; i < objc; i+=2) {

	if (Tcl_GetIndexFromObj(interp, objv[i], options, "option",
				0, &indx) != TCL_OK) {
	    return TCL_ERROR;
	}
	switch (indx) {

	case COPTION_ENCODING:
	    /* 
	     * Encoding - report "not implemented" unless the encoding
	     * would not be changed.
	     */

	    newEncoding = Tcl_GetEncoding(interp, Tcl_GetString(objv[i+1]));
	    if (newEncoding == NULL) {
		return TCL_ERROR;
	    }
	    sysEncoding = Tcl_GetEncoding(interp, NULL);
	    Tcl_FreeEncoding(newEncoding);
	    if (sysEncoding != NULL) {
		Tcl_FreeEncoding(sysEncoding);
	    }
	    if (newEncoding != sysEncoding) {
		Tcl_SetObjResult(interp,
				 Tcl_NewStringObj("optional function "
						  "not implemented", -1));
		Tcl_SetErrorCode(interp, "TDBC", "ODBC", "HYC00", "-1", NULL);
		return TCL_ERROR;
	    }
	    break;

	case COPTION_ISOLATION:
	    /* Transaction isolation level */

	    if (LookupOdbcConstant(interp, OdbcIsolationLevels,
				   "isolation level", objv[i+1],
				   &isol) != TCL_OK) {
		return TCL_ERROR;
	    }
	    mode = isol;
	    rc = SQLSetConnectAttr(hDBC, SQL_ATTR_TXN_ISOLATION,
				   (SQLPOINTER)mode, 0);
	    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		TransferSQLError(interp, SQL_HANDLE_DBC, hDBC, 
				 "(setting isolation level of connection)");
		return TCL_ERROR;
	    }
	    break;

	case COPTION_PARENT:
	    /* Parent window for connection dialog */

#ifdef USE_TK
	    /* Make sure we haven't connected already */

	    if (connectFlagsPtr == NULL || hParentWindowPtr == NULL) {
		Tcl_SetObjResult(interp,
				 Tcl_NewStringObj("-parent option cannot "
						  "be used after connection "
						  "is established", -1));
		Tcl_SetErrorCode(interp, "TDBC", "ODBC", "HY010", "-1", NULL);
		return TCL_ERROR;
	    }

	    /* Make sure that Tk is present and its Stubs are initialized */

	    if (Tcl_PkgRequire(interp, "Tk", TK_VERSION, 0) == NULL) {
		Tcl_ResetResult(interp);
		Tcl_SetObjResult(interp,
				 Tcl_NewStringObj("cannot use -parent "
						  "option because Tk is not "
						  "loaded", -1));
		Tcl_SetErrorCode(interp, "TDBC", "ODBC", "HY000", "-1", NULL);
		return TCL_ERROR;
	    }
	    Tcl_MutexLock(&hEnvMutex);
	    if (!tkStubsInited) {
		if (Tk_InitStubs(interp, TK_VERSION, 0) == NULL) {
		    Tcl_MutexUnlock(&hEnvMutex);
		    return TCL_ERROR;
		}
		tkStubsInited = 1;
	    }
	    Tcl_MutexUnlock(&hEnvMutex);

	    /* Find the parent window, and get its HWND if possible */

	    if ((tkwin = Tk_NameToWindow(interp, Tcl_GetString(objv[i+1]),
					 Tk_MainWindow(interp))) == NULL) {
		return TCL_ERROR;
	    }
	    Tk_MakeWindowExist(tkwin);
#ifdef _WIN32
	    *hParentWindowPtr = Tk_GetHWND(Tk_WindowId(tkwin));
#else
	    *hParentWindowPtr = (HWND) 1;
#endif
	    *connectFlagsPtr = SQL_DRIVER_COMPLETE_REQUIRED;
	    break;

#else /* not USE_TK */
	    
	    /* Tk is not present at build time. Complain if -parent is used */

	    Tcl_SetObjResult(interp,
			     Tcl_NewStringObj("cannot use -parent option "
					      "because tdbc::odbc was built "
					      "without Tk", -1));
	    Tcl_SetErrorCode(interp, "TDBC", "ODBC", "HY000", "-1", NULL);
	    return TCL_ERROR;
#endif /* USE_TK */
	    
	case COPTION_READONLY:
	    /* read-only indicator */
	    
	    if (Tcl_GetBooleanFromObj(interp, objv[i+1], &j) != TCL_OK) {
		return TCL_ERROR;
	    }
	    if (j) {
		mode = SQL_MODE_READ_ONLY;
	    } else {
		mode = SQL_MODE_READ_WRITE;
	    }
	    rc = SQLSetConnectAttr(hDBC, SQL_ATTR_ACCESS_MODE,
				   (SQLPOINTER)mode, 0);
	    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		TransferSQLError(interp, SQL_HANDLE_DBC, hDBC, 
				 "(setting access mode of connection)");
		return TCL_ERROR;
	    }
	    break;

	case COPTION_TIMEOUT:
	    /* timeout value */

	    if (Tcl_GetIntFromObj(interp, objv[i+1], &j) != TCL_OK) {
		return TCL_ERROR;
	    }
	    seconds = (SQLINTEGER)((j + 999) / 1000);
	    rc = SQLSetConnectAttr(hDBC, SQL_ATTR_CONNECTION_TIMEOUT,
				   (SQLPOINTER)seconds, 0);
	    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		/* 
		 * A failure is OK if the SQL state is "Optional
		 * Function Not Implemented" and we were trying to set
		 * a zero timeout.
		 */
		if (!SQLStateIs(SQL_HANDLE_DBC, hDBC, "HYC00")
		    || seconds != 0) {
		    TransferSQLError(interp, SQL_HANDLE_DBC, hDBC, 
				     "(setting access mode of connection)");
		    return TCL_ERROR;
		}
	    }
	    break;
	}
    }
    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * ConnectionInitMethod --
 *
 *	Initializer for ::tdbc::odbc::connection, which represents a
 *	database connection.
 *
 * Results:
 *	Returns a standard Tcl result.
 *
 * The ConnectionInitMethod takes the same arguments that the connection's
 * constructor does, and attempts to connect to the database. 
 *
 *-----------------------------------------------------------------------------
 */

static int
ConnectionInitMethod(
    ClientData clientData,	/* Environment handle */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext objectContext, /* Object context */
    int objc,			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    PerInterpData* pidata = (PerInterpData*) clientData;
				/* Per-interp data for the ODBC package */
    Tcl_Object thisObject;	/* The current connection object */
    SQLHDBC hDBC;		/* Handle to the database connection */
    SQLRETURN rc;		/* Return code from ODBC calls */
    HWND hParentWindow = NULL;	/* Windows handle of the main window */
    SQLWCHAR* connectionStringReq;
				/* Connection string requested by the caller */
    int connectionStringReqLen;
				/* Length of the requested connection string */
    SQLWCHAR connectionString[1025];
				/* Connection string actually used */
    SQLSMALLINT connectionStringLen;
				/* Length of the actual connection string */
    Tcl_DString connectionStringDS;
				/* Connection string converted to UTF-8 */
    SQLUSMALLINT connectFlags = SQL_DRIVER_NOPROMPT;
				/* Driver options */
    ConnectionData *cdata;	/* Client data for the connection object */

    thisObject = Tcl_ObjectContextObject(objectContext);

    /*
     * Check param count
     */

    if (objc < 3 || (objc%2) != 1) {
	Tcl_WrongNumArgs(interp, 2, objv,
			 "connection-string ?-option value?...");
	return TCL_ERROR;
    }

    /*
     * Allocate a connection handle
     */

    rc = SQLAllocHandle(SQL_HANDLE_DBC, pidata->hEnv, (SQLHANDLE*) &hDBC);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	TransferSQLError(interp, SQL_HANDLE_ENV, pidata->hEnv, 
			 "(allocating connection handle)");
	return TCL_ERROR;
    }

    /*
     * Grab configuration options.
     */

    if (objc > 3 
	&& ConfigureConnection(interp, hDBC, pidata, objc-3, objv+3,
			       &connectFlags, &hParentWindow) != TCL_OK) {
	SQLFreeHandle(SQL_HANDLE_DBC, hDBC);
	return TCL_ERROR;
    }
    
    /*
     * Connect to the database (SQLConnect, SQLDriverConnect, SQLBrowseConnect)
     */

    connectionStringReq = GetWCharStringFromObj(objv[2],
						&connectionStringReqLen);
    rc = SQLDriverConnectW(hDBC, hParentWindow, connectionStringReq,
			   (SQLSMALLINT) connectionStringReqLen,
			   connectionString, 1024, &connectionStringLen,
			   connectFlags);
    ckfree((char*) connectionStringReq);
    if (rc == SQL_NO_DATA) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj("operation cancelled", -1));
	return TCL_ERROR;
    } else if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	TransferSQLError(interp, SQL_HANDLE_DBC, hDBC,
			 "(connecting to database)");
	SQLFreeHandle(SQL_HANDLE_DBC, hDBC);
	return TCL_ERROR;
    }

    /* Attach data about the connection to the object metadata */

    cdata = (ConnectionData*) ckalloc(sizeof(ConnectionData));
    cdata->refCount = 1;
    cdata->pidata = pidata;
    IncrPerInterpRefCount(pidata);
    Tcl_DStringInit(&connectionStringDS);
    DStringAppendWChars(&connectionStringDS,
			connectionString, connectionStringLen);
    cdata->connectionString =
	Tcl_NewStringObj(Tcl_DStringValue(&connectionStringDS),
			 Tcl_DStringLength(&connectionStringDS));
    Tcl_IncrRefCount(cdata->connectionString);
    Tcl_DStringFree(&connectionStringDS);
    cdata->hDBC = hDBC;
    cdata->flags = CONNECTION_FLAG_AUTOCOMMIT;
    Tcl_ObjectSetMetadata(thisObject, &connectionDataType, (ClientData) cdata);
    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * ConnectionBeginTransactionMethod --
 *
 *	Method that requests that following operations on an OBBC connection
 *	be executed as an atomic transaction.
 *
 * Usage:
 *	$connection begintransaction
 *
 * Parameters:
 *	None.
 *
 * Results:
 *	Returns an empty result if successful, and throws an error otherwise.
 *
 *-----------------------------------------------------------------------------
*/

static int
ConnectionBeginTransactionMethod(
    ClientData clientData,	/* Unused */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext objectContext, /* Object context */
    int objc,			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    Tcl_Object thisObject = Tcl_ObjectContextObject(objectContext);
				/* The current connection object */
    ConnectionData* cdata = (ConnectionData*)
	Tcl_ObjectGetMetadata(thisObject, &connectionDataType);

    /* Check parameters */

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 2, objv, "");
	return TCL_ERROR;
    }

    /* Reject attempts at nested transactions */

    if (cdata->flags & CONNECTION_FLAG_XCN_ACTIVE) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj("ODBC does not support "
						  "nested transactions", -1));
	Tcl_SetErrorCode(interp, "TDBC", "ODBC", "HYC00", "-1", NULL);
	return TCL_ERROR;
    }
    cdata->flags |= CONNECTION_FLAG_XCN_ACTIVE;

    /* Turn off autocommit for the duration of the transaction */

    if (cdata->flags & CONNECTION_FLAG_AUTOCOMMIT) {
	if (SetAutocommitFlag(interp, cdata, 0) != TCL_OK) {
	    return TCL_ERROR;
	}
	cdata->flags &= ~CONNECTION_FLAG_AUTOCOMMIT;
    }

    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * ConnectionConfigureMethod --
 *
 *	Method that changes the configuration of an ODBC connection
 *
 * Usage:
 *	$connection configure
 * -or- $connection configure -option
 * -or- $connection configure ?-option value?...
 * 
 * Parameters:
 *	Alternating options and values
 *
 * Results:
 *	With no arguments, returns a complete list of configuration options.
 *	With a single argument, returns the value of the given configuration
 *	option.  With two or more arguments, sets the given configuration
 *	options to the given values.
 *
 *-----------------------------------------------------------------------------
 */

static int
ConnectionConfigureMethod(
    ClientData clientData,	/* Completion type */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext objectContext, /* Object context */
    int objc,			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    Tcl_Object thisObject = Tcl_ObjectContextObject(objectContext);
				/* The current connection object */
    ConnectionData* cdata = (ConnectionData*)
	Tcl_ObjectGetMetadata(thisObject, &connectionDataType);
				/* Instance data */

    /* Check parameters */

    if (objc != 2 && objc != 3 && (objc%2) != 0) {
	Tcl_WrongNumArgs(interp, 2, objv,
			 "?" "?-option? value? ?-option value?...");
	return TCL_ERROR;
    }

    return ConfigureConnection(interp, cdata->hDBC, cdata->pidata,
			       objc-2, objv+2, NULL, NULL);
}

/*
 *-----------------------------------------------------------------------------
 *
 * ConnectionEndXcnMethod --
 *
 *	Method that requests that a pending transaction against a database
 * 	be committed or rolled back.
 *
 * Usage:
 *	$connection commit
 * -or- $connection rollback
 * 
 * Parameters:
 *	None.
 *
 * Results:
 *	Returns an empty Tcl result if successful, and throws an error
 *	otherwise.
 *
 *-----------------------------------------------------------------------------
 */

static int
ConnectionEndXcnMethod(
    ClientData clientData,	/* Completion type */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext objectContext, /* Object context */
    int objc,			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    SQLSMALLINT completionType = (SQLSMALLINT) (int) (clientData);
    Tcl_Object thisObject = Tcl_ObjectContextObject(objectContext);
				/* The current connection object */
    ConnectionData* cdata = (ConnectionData*)
	Tcl_ObjectGetMetadata(thisObject, &connectionDataType);
				/* Instance data */
    SQLRETURN rc;		/* Result code from ODBC operations */

    /* Check parameters */

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 2, objv, "");
	return TCL_ERROR;
    }

    /* Reject the request if no transaction is in progress */

    if (!(cdata->flags & CONNECTION_FLAG_XCN_ACTIVE)) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj("no transaction is in "
						  "progress", -1));
	Tcl_SetErrorCode(interp, "TDBC", "ODBC", "HY010", "-1", NULL);
	return TCL_ERROR;
    }

    /* End transaction, turn off "transaction in progress", and report status */

    rc = SQLEndTran(SQL_HANDLE_DBC, cdata->hDBC, completionType);
    cdata->flags &= ~ CONNECTION_FLAG_XCN_ACTIVE;
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	TransferSQLError(interp, SQL_HANDLE_DBC, cdata->hDBC,
			 "(ending the transaction)");
	return TCL_ERROR;
    }
    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * ConnectionHasWvarcharMethod --
 *
 *	Private method that informs the code whether the connection supports
 *	WVARCHAR strings.
 *
 * Usage:
 *	$connection HasWvarchar boolean
 *
 * Parameters:
 *	boolean - 1 if the connection supports WVARCHAR, 0 otherwise
 *
 * Results:
 *	Returns an empty Tcl result.
 *
 *-----------------------------------------------------------------------------
 */

static int
ConnectionHasWvarcharMethod(
    ClientData clientData,	/* Completion type */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext objectContext, /* Object context */
    int objc,			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    Tcl_Object thisObject = Tcl_ObjectContextObject(objectContext);
				/* The current connection object */
    ConnectionData* cdata = (ConnectionData*)
	Tcl_ObjectGetMetadata(thisObject, &connectionDataType);
				/* Instance data */
    int flag;

    /* Check parameters */

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 2, objv, "flag");
	return TCL_ERROR;
    }
    if (Tcl_GetBooleanFromObj(interp, objv[2], &flag) != TCL_OK) {
	return TCL_ERROR;
    }
    if (flag) {
	cdata->flags |= CONNECTION_FLAG_HAS_WVARCHAR;
    } else {
	cdata->flags &= ~CONNECTION_FLAG_HAS_WVARCHAR;
    }
    return TCL_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SetAutocommitFlag --
 *
 *	Turns autocommit on or off at the ODBC level.
 *
 * Results:
 *	Returns TCL_OK if successful, TCL_ERROR otherwise. Stores error message
 *	in the interpreter.
 *
 *-----------------------------------------------------------------------------
 */

static int
SetAutocommitFlag(
    Tcl_Interp* interp,		/* Tcl interpreter */
    ConnectionData* cdata,	/* Instance data for the connection */
    SQLINTEGER flag		/* Auto-commit indicator */
) {
    SQLRETURN rc;
    rc = SQLSetConnectAttr(cdata->hDBC, SQL_ATTR_AUTOCOMMIT,
			   (SQLPOINTER) flag, 0);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	TransferSQLError(interp, SQL_HANDLE_DBC, cdata->hDBC,
			 "(changing the 'autocommit' attribute)");
	return TCL_ERROR;
    }
    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * DeleteCmd --
 *
 *	Callback executed when the initialization method of the connection
 *	class is deleted.
 *
 * Side effects:
 *	Dismisses the environment, which has the effect of shutting
 *	down ODBC when it is no longer required.
 *
 *-----------------------------------------------------------------------------
 */

static void
DeleteCmd (
    ClientData clientData	/* Environment handle */
) {
    PerInterpData* pidata = (PerInterpData*) clientData;
    DecrPerInterpRefCount(pidata);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CloneCmd --
 *
 *	Callback executed when any of the ODBC client methods is cloned.
 *
 * Results:
 *	Returns TCL_OK to allow the method to be copied.
 *
 * Side effects:
 *	Obtains a fresh copy of the environment handle, to keep the
 *	refcounts accurate
 *
 *-----------------------------------------------------------------------------
 */

static int
CloneCmd(
    Tcl_Interp* interp,		/* Tcl interpreter */
    ClientData oldClientData,	/* Environment handle to be discarded */
    ClientData* newClientData	/* New environment handle to be used */
) {
    *newClientData = GetHEnv(NULL);
    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * DeleteConnectionMetadata, DeleteConnection --
 *
 *	Cleans up when a database connection is deleted.  
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Terminates the connection and frees all system resources associated
 *	with it.
 *
 *-----------------------------------------------------------------------------
 */

static void
DeleteConnectionMetadata(
    ClientData clientData	/* Instance data for the connection */
) {
    DecrConnectionRefCount((ConnectionData*)clientData);
}
static void
DeleteConnection(
    ConnectionData* cdata	/* Instance data for the connection */
) {
    /* 
     * All SQL errors are ignored here because we can't do anything
     * about them, anyway.
     */

    if (cdata->flags & CONNECTION_FLAG_XCN_ACTIVE) {
	SQLEndTran(SQL_HANDLE_DBC, cdata->hDBC, SQL_ROLLBACK);
    }
    SQLDisconnect(cdata->hDBC);
    SQLFreeHandle(SQL_HANDLE_DBC, cdata->hDBC);
    Tcl_DecrRefCount(cdata->connectionString);
    DecrPerInterpRefCount(cdata->pidata);
    ckfree((char*) cdata);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CloneConnection --
 *
 *	Attempts to clone an ODBC connection's metadata.
 *
 * Results:
 *	Returns the new metadata
 *
 * At present, we don't attempt to clone connections - it's not obvious
 * that such an action would ever even make sense.  Instead, we return NULL
 * to indicate that the metadata should not be cloned. (Note that this
 * action isn't right, either. What *is* right is to indicate that the object
 * is not clonable, but the API gives us no way to do that.
 *
 *-----------------------------------------------------------------------------
 */

static int
CloneConnection(
    Tcl_Interp* interp,		/* Tcl interpreter for error reporting */
    ClientData metadata,	/* Metadata to be cloned */
    ClientData* newMetaData	/* Where to put the cloned metadata */
) {
    Tcl_SetObjResult(interp,
		     Tcl_NewStringObj("ODBC connections are not clonable", -1));
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * NewStatement --
 *
 *	Creates an empty object to hold statement data.
 *
 * Results:
 *	Returns a pointer to the newly-created object.
 *
 *-----------------------------------------------------------------------------
 */

static StatementData*
NewStatement(
    ConnectionData* cdata	/* Instance data for the connection */
) {
    StatementData* sdata = (StatementData*) ckalloc(sizeof(StatementData));
    sdata->refCount = 1;
    sdata->cdata = cdata;
    IncrConnectionRefCount(cdata);
    sdata->subVars = Tcl_NewObj();
    Tcl_IncrRefCount(sdata->subVars);
    sdata->hStmt = SQL_NULL_HANDLE;
    sdata->nativeSqlW = NULL;
    sdata->nativeSqlLen = 0;
    sdata->nativeMatchPatternW = NULL;
    sdata->nativeMatchPatLen = 0;
    sdata->params = NULL;
    sdata->resultColNames = NULL;
    sdata->results = NULL;
    sdata->flags = 0;
    sdata->typeNum = SQL_ALL_TYPES;
    return sdata;
}

/*
 *-----------------------------------------------------------------------------
 *
 * StatementInitMethod --
 *
 *	C-level initialization for the object representing an ODBC prepared
 *	statement.
 *
 * Parameters:
 *	Accepts a 4-element 'objv': $object init $connection $statementText,
 *	where $connection is the ODBC connection object, and $statementText
 *	is the text of the statement to prepare.
 *
 * Results:
 *	Returns a standard Tcl result
 *
 * Side effects:
 *	Prepares the statement, and stores it (plus a reference to the
 *	connection) in instance metadata.
 *
 *-----------------------------------------------------------------------------
 */

static int
StatementInitMethod(
    ClientData clientData,	/* Not used */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context,	/* Object context  */
    int objc, 			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {

    Tcl_Object thisObject;	/* The current statement object */
    Tcl_Object connectionObject;
				/* The database connection as a Tcl_Object */
    ConnectionData* cdata;	/* The connection object's data */
    StatementData* sdata;	/* The statement's object data */
    Tcl_Obj* tokens;		/* The tokens of the statement to be prepared */
    int tokenc;			/* Length of the 'tokens' list */
    Tcl_Obj** tokenv;		/* Exploded tokens from the list */
    Tcl_Obj* nativeSql;		/* SQL statement mapped to ODBC form */
    char* tokenStr;		/* Token string */
    int tokenLen;		/* Length of a token */
    RETCODE rc;			/* Return code from ODBC */
    SQLSMALLINT nParams;	/* Number of parameters in the ODBC statement */
    int i, j;

    /* Find the connection object, and get its data. */

    thisObject = Tcl_ObjectContextObject(context);
    if (objc != 4) {
	Tcl_WrongNumArgs(interp, 2, objv, "connection statementText");
	return TCL_ERROR;
    }

    connectionObject = Tcl_GetObjectFromObj(interp, objv[2]);
    if (connectionObject == NULL) {
	return TCL_ERROR;
    }
    cdata = (ConnectionData*) Tcl_ObjectGetMetadata(connectionObject,
						    &connectionDataType);
    if (cdata == NULL) {
	Tcl_AppendResult(interp, Tcl_GetString(objv[2]),
			 " does not refer to an ODBC connection", NULL);
	return TCL_ERROR;
    }

    /*
     * Allocate an object to hold data about this statement
     */

    sdata = NewStatement(cdata);

    /* Tokenize the statement */

    tokens = Tdbc_TokenizeSql(interp, Tcl_GetString(objv[3]));
    if (tokens == NULL) {
	goto freeSData;
    }
    Tcl_IncrRefCount(tokens);

    /*
     * Rewrite the tokenized statement to ODBC syntax. Reject the
     * statement if it is actually multiple statements.
     */

    if (Tcl_ListObjGetElements(interp, tokens, &tokenc, &tokenv) != TCL_OK) {
	goto freeTokens;
    }
    nativeSql = Tcl_NewObj();
    Tcl_IncrRefCount(nativeSql);
    for (i = 0; i < tokenc; ++i) {
	tokenStr = Tcl_GetStringFromObj(tokenv[i], &tokenLen);
	
	switch (tokenStr[0]) {
	case '$':
	case ':':
	case '@':
	    Tcl_AppendToObj(nativeSql, "?", 1);
	    Tcl_ListObjAppendElement(NULL, sdata->subVars, 
				     Tcl_NewStringObj(tokenStr+1, tokenLen-1));
	    break;

	case ';':
	    Tcl_SetObjResult(interp,
			     Tcl_NewStringObj("tdbc::odbc"
					      " does not support semicolons "
					      "in statements", -1));
	    goto freeNativeSql;
	    break; 

	default:
	    Tcl_AppendToObj(nativeSql, tokenStr, tokenLen);
	    break;

	}
    }

    /* Allocate an ODBC statement handle, and prepare the statement */

    sdata->nativeSqlW = GetWCharStringFromObj(nativeSql, &sdata->nativeSqlLen);
    sdata->hStmt = AllocAndPrepareStatement(interp, sdata);
    if (sdata->hStmt == SQL_NULL_HANDLE) {
	goto freeNativeSql;
    }

    /* Determine the number of parameters that ODBC thinks are in the 
     * statement. */

    Tcl_ListObjLength(NULL, sdata->subVars, &i);
    sdata->params = (ParamData*) ckalloc(i * sizeof(ParamData));
    for (j = 0; j < i; ++j) {
	/* 
	 * Supply defaults in case the driver doesn't support introspection
	 * of parameters.  Since not all drivers do WVARCHAR, VARCHAR
	 * appears to be the only workable option.
	 */
	if (cdata->flags && CONNECTION_FLAG_HAS_WVARCHAR) {
	    sdata->params[j].dataType = SQL_WVARCHAR;
	} else {
	    sdata->params[j].dataType = SQL_VARCHAR;
	}
	sdata->params[j].precision = 255;
	sdata->params[j].scale = 0;
	sdata->params[j].nullable = SQL_NULLABLE_UNKNOWN;
	sdata->params[j].flags = PARAM_IN;
    }
    rc = SQLNumParams(sdata->hStmt, &nParams);
    if (rc == SQL_SUCCESS && rc == SQL_SUCCESS_WITH_INFO) {
	if (nParams != i) {
	    Tcl_SetObjResult(interp,
			     Tcl_NewStringObj("The SQL statement appears "
					      "to contain parameters in "
					      "native SQL syntax. You need "
					      "to replace them with ones "
					      "in ':variableName' form.", -1));
	    Tcl_SetErrorCode(interp, "TDBC", "ODBC", "HY000", "-1");
	    goto freeNativeSql;
	}

	/* 
	 * Try to describe the parameters for the sake of consistency
	 * in conversion and efficiency in execution. 
	 */

	for (i = 0; i < nParams; ++i) {
	    rc = SQLDescribeParam(sdata->hStmt, i+1,
				  &(sdata->params[i].dataType),
				  &(sdata->params[i].precision),
				  &(sdata->params[i].scale),
				  &(sdata->params[i].nullable));
	    if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
		/* 
		 * TODO: The next statement doesn't work for stored
		 * procedure calls, which are the only case where
		 * output and inout parameters exist. They need to
		 * go through a separate path if they're to be managed
		 * properly. Right now, we presume that all params
		 * are inputs.
		 */
		sdata->params[i].flags = PARAM_IN | PARAM_KNOWN;
	    } else {
		/* 
		 * Supply defaults in case the driver doesn't support 
		 * introspection of parameters. Again, not all drivers can
		 * handle WVARCHAR, so VARCHAR seems to be the only
		 * workable option.
		 */
		if (cdata->flags && CONNECTION_FLAG_HAS_WVARCHAR) {
		    sdata->params[j].dataType = SQL_WVARCHAR;
		} else {
		    sdata->params[j].dataType = SQL_VARCHAR;
		}
		sdata->params[j].precision = 255;
		sdata->params[j].scale = 0;
		sdata->params[j].nullable = SQL_NULLABLE_UNKNOWN;
		sdata->params[j].flags = PARAM_IN;
	    }
	}
    }

    /* Attach the current statement data as metadata to the current object */

    Tcl_ObjectSetMetadata(thisObject, &statementDataType, (ClientData) sdata);
    return TCL_OK;

    /* On error, unwind all the resource allocations */

 freeNativeSql:
    Tcl_DecrRefCount(nativeSql);
 freeTokens:
    Tcl_DecrRefCount(tokens);
 freeSData:
    DecrStatementRefCount(sdata);
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * StatementParamListMethod --
 *
 *	Lists the parameters to an ODBC statement
 *
 * Usage:
 *	$statement ParamList
 *
 * Results:
 *	Returns a standard Tcl result that is a list of alternating
 *	elements: paramName flags typeNumber precision scale nullable
 *
 *-----------------------------------------------------------------------------
 */

static int
StatementParamListMethod(
    ClientData clientData,	/* Not used */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context,	/* Object context  */
    int objc, 			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    Tcl_Object thisObject = Tcl_ObjectContextObject(context);
				/* The current statement object */
    StatementData* sdata;	/* The current statement */
    Tcl_Obj **paramNames;	/* Parameter list to the current statement */
    int nParams;		/* Parameter count for the current statement */
    int i;			/* Current parameter index */
    Tcl_Obj* retval;		/* Return value from this command */

    sdata = (StatementData*) Tcl_ObjectGetMetadata(thisObject,
						   &statementDataType);

    retval = Tcl_NewObj();
    if (sdata->subVars != NULL) {
	Tcl_ListObjGetElements(NULL, sdata->subVars, &nParams, &paramNames);
	for (i = 0; i < nParams; ++i) {
	    ParamData* pd = sdata->params + i;
	    Tcl_ListObjAppendElement(NULL, retval, paramNames[i]);
	    Tcl_ListObjAppendElement(NULL, retval, Tcl_NewIntObj(pd->flags));
	    Tcl_ListObjAppendElement(NULL, retval, Tcl_NewIntObj(pd->dataType));
	    Tcl_ListObjAppendElement(NULL, retval,
				     Tcl_NewIntObj(pd->precision));
	    Tcl_ListObjAppendElement(NULL, retval, Tcl_NewIntObj(pd->scale));
	    Tcl_ListObjAppendElement(NULL, retval, Tcl_NewIntObj(pd->nullable));
	}
    }
    Tcl_SetObjResult(interp, retval);
    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * StatementParamtypeMethod --
 *
 *	Defines a parameter type in an ODBC statement.
 *
 * Usage:
 *	$statement paramtype paramName ?direction? type ?precision ?scale??
 *
 * Results:
 *	Returns a standard Tcl result.
 *
 * Side effects:
 *	Updates the description of the given parameter.
 *
 *-----------------------------------------------------------------------------
 */

static int
StatementParamtypeMethod(
    ClientData clientData,	/* Not used */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context,	/* Object context  */
    int objc, 			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    Tcl_Object thisObject = Tcl_ObjectContextObject(context);
				/* The current statement object */
    StatementData* sdata;	/* The current statement */
    int matchCount = 0;		/* The number of variables in the given
				 * statement that match the given one */
    int nParams;		/* Number of parameters to the statement */
    const char* paramName;	/* Name of the parameter being set */
    Tcl_Obj* targetNameObj;	/* Name of the ith parameter in the statement */
    const char* targetName;	/* Name of a candidate parameter in the
				 * statement */
    Tcl_Obj* errorObj;		/* Error message */
    int i;
    SQLSMALLINT dir = PARAM_IN | PARAM_KNOWN;
				/* Direction of parameter transmssion */
    SQLSMALLINT odbcType = SQL_VARCHAR;
				/* ODBC type of the parameter */
    int precision = 0;		/* Length of the parameter */
    int scale = 0;		/* Precision of the parameter */
    sdata = (StatementData*) Tcl_ObjectGetMetadata(thisObject,
						   &statementDataType);
    /* Check parameters */

    if (objc < 4) {
	goto wrongNumArgs;
    }
    i = 3;
    if (LookupOdbcConstant(NULL, OdbcParamDirections, "direction", objv[i],
			   &dir) == TCL_OK) {
	++i;
    }
    if (i >= objc) {
	goto wrongNumArgs;
    }
    if (LookupOdbcType(interp, objv[i], &odbcType) == TCL_OK) {
	++i;
    } else {
	return TCL_ERROR;
    }
    if (i < objc) {
	if (Tcl_GetIntFromObj(interp, objv[i], &precision) == TCL_OK) {
	    ++i;
	} else {
	    return TCL_ERROR;
	}
    }
    if (i < objc) {
	if (Tcl_GetIntFromObj(interp, objv[i], &scale) == TCL_OK) {
	    ++i;
	} else {
	    return TCL_ERROR;
	}
    }
    if (i != objc) {
	goto wrongNumArgs;
    }

    Tcl_ListObjLength(NULL, sdata->subVars, &nParams);
    paramName = Tcl_GetString(objv[2]);
    for (i = 0; i < nParams; ++i) {
	Tcl_ListObjIndex(NULL, sdata->subVars, i, &targetNameObj);
	targetName = Tcl_GetString(targetNameObj);
	if (!strcmp(paramName, targetName)) {
	    ++matchCount;

	    sdata->params[i].flags = dir;
	    sdata->params[i].dataType = odbcType;
	    sdata->params[i].precision = precision;
	    sdata->params[i].scale = scale;
	    sdata->params[i].nullable = 1;
				/* TODO - Update TIP so that user
				 * can specify nullable? */
	}
    }
    if (matchCount == 0) {
	errorObj = Tcl_NewStringObj("unknown parameter \"", -1);
	Tcl_AppendToObj(errorObj, paramName, -1);
	Tcl_AppendToObj(errorObj, "\": must be ", -1);
	for (i = 0; i < nParams; ++i) {
	    Tcl_ListObjIndex(NULL, sdata->subVars, i, &targetNameObj);
	    Tcl_AppendObjToObj(errorObj, targetNameObj);
	    if (i < nParams-2) {
		Tcl_AppendToObj(errorObj, ", ", -1);
	    } else if (i == nParams-2) {
		Tcl_AppendToObj(errorObj, " or ", -1);
	    }
	}
	Tcl_SetObjResult(interp, errorObj);
	return TCL_ERROR;
    }

    return TCL_OK;
 wrongNumArgs:
    Tcl_WrongNumArgs(interp, 2, objv,
		     "name ?direction? type ?precision ?scale??");
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * TablesStatementInitMethod --
 *
 *	C-level initialization for the object representing an ODBC query
 *	for table metadata
 *
 * Parameters:
 *	Accepts a 4-element 'objv': $object init $connection $pattern,
 *	where $connection is the ODBC connection object, and $pattern
 *	is the pattern to match table names.
 *
 * Results:
 *	Returns a standard Tcl result
 *
 * Side effects:
 *	Prepares the statement, and stores it (plus a reference to the
 *	connection) in instance metadata.
 *
 *-----------------------------------------------------------------------------
 */

static int
TablesStatementInitMethod(
    ClientData clientData,	/* Not used */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context,	/* Object context  */
    int objc, 			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {

    Tcl_Object thisObject;	/* The current statement object */
    Tcl_Object connectionObject;
				/* The database connection as a Tcl_Object */
    ConnectionData* cdata;	/* The connection object's data */
    StatementData* sdata;	/* The statement's object data */
    RETCODE rc;			/* Return code from ODBC */

    /* Find the connection object, and get its data. */

    thisObject = Tcl_ObjectContextObject(context);
    if (objc != 4) {
	Tcl_WrongNumArgs(interp, 2, objv, "connection pattern");
	return TCL_ERROR;
    }

    connectionObject = Tcl_GetObjectFromObj(interp, objv[2]);
    if (connectionObject == NULL) {
	return TCL_ERROR;
    }
    cdata = (ConnectionData*) Tcl_ObjectGetMetadata(connectionObject,
						    &connectionDataType);
    if (cdata == NULL) {
	Tcl_AppendResult(interp, Tcl_GetString(objv[2]),
			 " does not refer to an ODBC connection", NULL);
	return TCL_ERROR;
    }

    /*
     * Allocate an object to hold data about this statement
     */

    sdata = NewStatement(cdata);

    /* Allocate an ODBC statement handle */

    rc = SQLAllocHandle(SQL_HANDLE_STMT, cdata->hDBC, &(sdata->hStmt));
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	TransferSQLError(interp, SQL_HANDLE_DBC, cdata->hDBC, 
			 "(allocating statement handle)");
	goto freeSData;
    }

    /* 
     * Stash the table pattern in the statement data, and set a flag that
     * that's what we have there.
     */

    sdata->nativeSqlW = GetWCharStringFromObj(objv[3], &(sdata->nativeSqlLen));
    sdata->nativeMatchPatternW = NULL;
    sdata->flags |= STATEMENT_FLAG_TABLES;

    /* Attach the current statement data as metadata to the current object */

    Tcl_ObjectSetMetadata(thisObject, &statementDataType, (ClientData) sdata);
    return TCL_OK;

    /* On error, unwind all the resource allocations */

 freeSData:
    DecrStatementRefCount(sdata);
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * ColumnsStatementInitMethod --
 *
 *	C-level initialization for the object representing an ODBC query
 *	for column metadata
 *
 * Parameters:
 *	Accepts a 5-element 'objv': $object init $connection $table $pattern,
 *	where $connection is the ODBC connection object, $table is the
 *	name of the table being queried, and $pattern is the pattern to
 *	match column names.
 *
 * Results:
 *	Returns a standard Tcl result
 *
 * Side effects:
 *	Prepares the statement, and stores it (plus a reference to the
 *	connection) in instance metadata.
 *
 *-----------------------------------------------------------------------------
 */

static int
ColumnsStatementInitMethod(
    ClientData clientData,	/* Not used */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context,	/* Object context  */
    int objc, 			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {

    Tcl_Object thisObject;	/* The current statement object */
    Tcl_Object connectionObject;
				/* The database connection as a Tcl_Object */
    ConnectionData* cdata;	/* The connection object's data */
    StatementData* sdata;	/* The statement's object data */
    RETCODE rc;			/* Return code from ODBC */

    /* Find the connection object, and get its data. */

    thisObject = Tcl_ObjectContextObject(context);
    if (objc != 5) {
	Tcl_WrongNumArgs(interp, 2, objv, "connection tableName pattern");
	return TCL_ERROR;
    }

    connectionObject = Tcl_GetObjectFromObj(interp, objv[2]);
    if (connectionObject == NULL) {
	return TCL_ERROR;
    }
    cdata = (ConnectionData*) Tcl_ObjectGetMetadata(connectionObject,
						    &connectionDataType);
    if (cdata == NULL) {
	Tcl_AppendResult(interp, Tcl_GetString(objv[2]),
			 " does not refer to an ODBC connection", NULL);
	return TCL_ERROR;
    }

    /*
     * Allocate an object to hold data about this statement
     */

    sdata = NewStatement(cdata);

    /* Allocate an ODBC statement handle */

    rc = SQLAllocHandle(SQL_HANDLE_STMT, cdata->hDBC, &(sdata->hStmt));
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	TransferSQLError(interp, SQL_HANDLE_DBC, cdata->hDBC, 
			 "(allocating statement handle)");
	goto freeSData;
    }

    /* 
     * Stash the table name and match pattern in the statement data,
     * and set a flag that that's what we have there.
     */

    sdata->nativeSqlW = GetWCharStringFromObj(objv[3], &(sdata->nativeSqlLen));
    sdata->nativeMatchPatternW =
	GetWCharStringFromObj(objv[4], &(sdata->nativeMatchPatLen));
    sdata->flags = STATEMENT_FLAG_COLUMNS;

    /* Attach the current statement data as metadata to the current object */


    Tcl_ObjectSetMetadata(thisObject, &statementDataType, (ClientData) sdata);
    return TCL_OK;

    /* On error, unwind all the resource allocations */

 freeSData:
    DecrStatementRefCount(sdata);
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * TypesStatementInitMethod --
 *
 *	C-level initialization for the object representing an ODBC query
 *	for data type metadata
 *
 * Parameters:
 *	Accepts a 3- or 4-element 'objv': $object init $connection ?$typeNum?
 *	where $connection is the ODBC connection object, and $typeNum,
 *	if present, makes the query match only the given type.
 *
 * Results:
 *	Returns a standard Tcl result
 *
 * Side effects:
 *	Prepares the statement, and stores it (plus a reference to the
 *	connection) in instance metadata.
 *
 *-----------------------------------------------------------------------------
 */

static int
TypesStatementInitMethod(
    ClientData clientData,	/* Not used */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context,	/* Object context  */
    int objc, 			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {

    Tcl_Object thisObject;	/* The current statement object */
    Tcl_Object connectionObject;
				/* The database connection as a Tcl_Object */
    ConnectionData* cdata;	/* The connection object's data */
    StatementData* sdata;	/* The statement's object data */
    RETCODE rc;			/* Return code from ODBC */
    int typeNum;		/* Data type number */

    /* Parse args */

    if (objc == 3) {
	typeNum = SQL_ALL_TYPES;
    } else if (objc == 4) {
	if (Tcl_GetIntFromObj(interp, objv[3], &typeNum) != TCL_OK) {
	    return TCL_ERROR;
	} 
    } else {
	Tcl_WrongNumArgs(interp, 2, objv, "connection ?typeNum?");
	return TCL_ERROR;
    }

    /* Find the connection object, and get its data. */

    thisObject = Tcl_ObjectContextObject(context);
    connectionObject = Tcl_GetObjectFromObj(interp, objv[2]);
    if (connectionObject == NULL) {
	return TCL_ERROR;
    }
    cdata = (ConnectionData*) Tcl_ObjectGetMetadata(connectionObject,
						    &connectionDataType);
    if (cdata == NULL) {
	Tcl_AppendResult(interp, Tcl_GetString(objv[2]),
			 " does not refer to an ODBC connection", NULL);
	return TCL_ERROR;
    }

    /*
     * Allocate an object to hold data about this statement
     */

    sdata = NewStatement(cdata);

    /* Allocate an ODBC statement handle */

    rc = SQLAllocHandle(SQL_HANDLE_STMT, cdata->hDBC, &(sdata->hStmt));
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	TransferSQLError(interp, SQL_HANDLE_DBC, cdata->hDBC, 
			 "(allocating statement handle)");
	goto freeSData;
    }

    /*
     * Stash the type number in the statement data, and set a flag
     * that that's what we have there.
     */

    sdata->typeNum = typeNum;
    sdata->flags = STATEMENT_FLAG_TYPES;

    /* Attach the current statement data as metadata to the current object */


    Tcl_ObjectSetMetadata(thisObject, &statementDataType, (ClientData) sdata);
    return TCL_OK;

    /* On error, unwind all the resource allocations */

 freeSData:
    DecrStatementRefCount(sdata);
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * DeleteStatementMetadata, DeleteStatement --
 *
 *	Cleans up when an ODBC statement is no longer required.
 *
 * Side effects:
 *	Frees all resources associated with the statement.
 *
 *-----------------------------------------------------------------------------
 */

static void
DeleteStatementMetadata(
    ClientData clientData	/* Instance data for the connection */
) {
    DecrStatementRefCount((StatementData*)clientData);
}
static void
DeleteStatement(
    StatementData* sdata	/* Metadata for the statement */
) {
    if (sdata->hStmt != SQL_NULL_HANDLE) {
	SQLFreeHandle(SQL_HANDLE_STMT, sdata->hStmt);
    }
    if (sdata->results != NULL) {
	ckfree((char*) sdata->results);
    }
    if (sdata->resultColNames != NULL) {
	Tcl_DecrRefCount(sdata->resultColNames);
    }
    if (sdata->params != NULL) {
	ckfree((char*) sdata->params);
    }
    Tcl_DecrRefCount(sdata->subVars);
    if (sdata->nativeSqlW != NULL) {
	ckfree((char*) sdata->nativeSqlW);
    }
    if (sdata->nativeMatchPatternW != NULL) {
	ckfree((char*) sdata->nativeMatchPatternW);
    }
    DecrConnectionRefCount(sdata->cdata);
    ckfree((char*)sdata);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CloneStatement --
 *
 *	Attempts to clone an ODBC statement's metadata.
 *
 * Results:
 *	Returns the new metadata
 *
 * At present, we don't attempt to clone statements - it's not obvious
 * that such an action would ever even make sense.  Instead, we return NULL
 * to indicate that the metadata should not be cloned. (Note that this
 * action isn't right, either. What *is* right is to indicate that the object
 * is not clonable, but the API gives us no way to do that.
 *
 *-----------------------------------------------------------------------------
 */

static int
CloneStatement(
    Tcl_Interp* interp,		/* Tcl interpreter for error reporting */
    ClientData metadata,	/* Metadata to be cloned */
    ClientData* newMetaData	/* Where to put the cloned metadata */
) {
    Tcl_SetObjResult(interp,
		     Tcl_NewStringObj("ODBC statements are not clonable", -1));
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * ResultSetInitMethod --
 *
 *	Constructs a new result set.
 *
 * Usage:
 *	$resultSet init statement ?dictionary?
 *
 * Parameters:
 *	dictionary -- Dictionary containing the substitutions for named
 *		      parameters in the given statement.
 *
 * Results:
 *	Returns a standard Tcl result.  On error, the interpreter result
 *	contains an appropriate message.
 *
 *-----------------------------------------------------------------------------
 */

static int
ResultSetInitMethod(
    ClientData clientData,	/* Not used */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context,	/* Object context  */
    int objc, 			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {

    Tcl_Object thisObject = Tcl_ObjectContextObject(context);
				/* The current result set object */
    Tcl_Object statementObject;	/* The current statement object */
    PerInterpData* pidata;	/* The per-interpreter data for this package */
    ConnectionData* cdata;	/* The ODBC connection object's data */
    StatementData* sdata;	/* The statement object's data */
    ResultSetData* rdata;	/* THe result set object's data */
    int nParams;		/* Number of substituted parameters in
				 * the statement */
    int nBound;			/* Number of substituted parameters that
				 * have been bound successfully */
    SQLSMALLINT dataType;	/* Data type of a parameter */
    Tcl_Obj* paramNameObj;	/* Name of a substituted parameter */
    const char* paramName;	/* Name of a substituted parameter */
    Tcl_Obj* paramValObj;	/* Value of a substituted parameter */
    const char* paramVal;	/* Value of a substituted parameter */
    int paramLen;		/* String length of the parameter value */
    Tcl_DString paramExternal;	/* Substituted parameter, converted to
				 * system encoding */
    int paramExternalLen;	/* Length of the substituted parameter
				 * after conversion */
    SQLRETURN rc;		/* Return code from ODBC calls */
    int i;

    /* Check parameter count */

    if (objc != 3 && objc != 4) {
	Tcl_WrongNumArgs(interp, 2, objv, "statement ?dictionary?");
	return TCL_ERROR;
    }

    /* Find the statement object, and get the statement data */

    statementObject = Tcl_GetObjectFromObj(interp, objv[2]);
    if (statementObject == NULL) {
	return TCL_ERROR;
    }
    sdata = (StatementData*) Tcl_ObjectGetMetadata(statementObject,
						   &statementDataType);
    if (sdata == NULL) {
	Tcl_AppendResult(interp, Tcl_GetString(objv[2]),
			 " does not refer to an ODBC statement", NULL);
	return TCL_ERROR;
    }

    /* 
     * If there is no transaction in progress, turn on auto-commit so that
     * this statement will execute directly.
     */

    cdata = sdata->cdata;
    if ((cdata->flags & (CONNECTION_FLAG_XCN_ACTIVE
			 | CONNECTION_FLAG_AUTOCOMMIT)) == 0) {
	cdata->flags |= CONNECTION_FLAG_AUTOCOMMIT;
	if (SetAutocommitFlag(interp, cdata, 1) != TCL_OK) {
	    return TCL_ERROR;
	}
    }
    pidata = cdata->pidata;

    /* Allocate an object to hold data about this result set */

    rdata = (ResultSetData*) ckalloc(sizeof(ResultSetData));
    rdata->refCount = 1;
    rdata->sdata = sdata;
    rdata->hStmt = NULL;
    IncrStatementRefCount(sdata);
    Tcl_ObjectSetMetadata(thisObject, &resultSetDataType, (ClientData) rdata);

    /* 
     * Find a statement handle that we can use to execute the SQL code.
     * If the main statement handle associated with the statement
     * is idle, we can use it.  Otherwise, we have to allocate and
     * prepare a fresh one.
     */

    if (sdata->flags & STATEMENT_FLAG_HSTMT_BUSY) {
	rdata->hStmt = AllocAndPrepareStatement(interp, sdata);
	if (rdata->hStmt == NULL) {
	    return TCL_ERROR;
	}
    } else {
	rdata->hStmt = sdata->hStmt;
	sdata->flags |= STATEMENT_FLAG_HSTMT_BUSY;
    }

    /* Allocate an array to hold SQLWCHAR strings with parameter data */

    Tcl_ListObjLength(NULL, sdata->subVars, &nParams);
    rdata->bindStrings = (SQLCHAR**) ckalloc(nParams * sizeof(SQLCHAR*));
    rdata->bindStringLengths = (SQLLEN*) ckalloc(nParams * sizeof(SQLLEN));
    for (i = 0; i < nParams; ++i) {
	rdata->bindStrings[i] = NULL;
	rdata->bindStringLengths[i] = SQL_NULL_DATA;
    }

    /* Bind the substituted parameters */

    for (nBound = 0; nBound < nParams; ++nBound) {
	Tcl_ListObjIndex(NULL, sdata->subVars, nBound, &paramNameObj);
	paramName = Tcl_GetString(paramNameObj);
	if (objc == 4) {

	    /* Param from a dictionary */

	    if (Tcl_DictObjGet(interp, objv[3], paramNameObj, &paramValObj)
		!= TCL_OK) {
		return TCL_ERROR;
	    }
	} else {

	    /* Param from a variable; check existence first */

	    Tcl_Obj* cmd[3];
	    int result;
	    int exists;
	    cmd[0] = pidata->literals[LIT_INFO]; Tcl_IncrRefCount(cmd[0]);
	    cmd[1] = pidata->literals[LIT_EXISTS]; Tcl_IncrRefCount(cmd[1]);
	    cmd[2] = paramNameObj; Tcl_IncrRefCount(cmd[2]);
	    result = Tcl_EvalObjv(interp, 3, cmd, TCL_EVAL_DIRECT);
	    if (result != TCL_OK) {
		return result;
	    }
	    if (Tcl_GetIntFromObj(interp, Tcl_GetObjResult(interp), &exists)
		!= TCL_OK) {
		/* Can happen only if someone overloaded ::info */
		return TCL_ERROR;
	    }
	    Tcl_ResetResult(interp);
	    Tcl_DecrRefCount(cmd[0]);
	    Tcl_DecrRefCount(cmd[1]);
	    Tcl_DecrRefCount(cmd[2]);
	    if (exists) {
		paramValObj = Tcl_GetVar2Ex(interp, paramName, NULL, 
					    TCL_LEAVE_ERR_MSG);
		if (paramValObj == NULL) {
		    return TCL_ERROR;
		}
	    } else {
		paramValObj = NULL;
	    }
	}

	/* 
	 * Choose the C->SQL data conversion based on the parameter type
	 */

	if (paramValObj != NULL) {

	    switch (sdata->params[nBound].dataType) {

	    case SQL_NUMERIC:
	    case SQL_DECIMAL:
	
		/* 
		 * A generic 'numeric' type may fit in an int, wide,
		 * or double, and gets converted specially if it does.
		 */
	
		if (sdata->params[nBound].scale == 0) {
		    if (sdata->params[nBound].precision < 10) {
			goto is_integer;
		    } else if (sdata->params[nBound].precision < 19) {
			goto is_wide;
		    } else {
			/*
			 * It is tempting to convert wider integers as bignums,
			 * but Tcl does not yet export its copy of libtommath
			 * into the public API.
			 */
			goto is_string;
		    }
		} else if (sdata->params[nBound].precision <= 15) {
		    goto is_float;
		} else {
		    goto is_string;
		}

	    case SQL_REAL:
	    case SQL_DOUBLE:
	    is_float:
		
		/* Pass floating point numbers through to SQL without
		 * conversion */

		rdata->bindStrings[nBound] =
		    (SQLCHAR*) ckalloc(sizeof(double));
		if (Tcl_GetDoubleFromObj(interp, paramValObj, 
					 (double*)(rdata->bindStrings[nBound]))
		    != TCL_OK) {
		    ckfree((char*)(rdata->bindStrings[nBound]));
		    goto is_string;
		}
		dataType = SQL_C_DOUBLE;
		paramExternalLen = sizeof(double);
		rdata->bindStringLengths[nBound] = paramExternalLen;
		break;

	    case SQL_BIGINT:
	    is_wide:

		/* Pass 64-bit integers through to SQL without conversion */

		rdata->bindStrings[nBound] =
		    (SQLCHAR*) ckalloc(sizeof(SQLBIGINT));
		if (Tcl_GetWideIntFromObj(interp, paramValObj, 
					  (SQLBIGINT*)
					  (rdata->bindStrings[nBound]))
		    != TCL_OK) {
		    ckfree((char*)(rdata->bindStrings[nBound]));
		    goto is_string;
		}
		dataType = SQL_C_SBIGINT;
		paramExternalLen = sizeof(SQLBIGINT);
		rdata->bindStringLengths[nBound] = paramExternalLen;
		break;

	    case SQL_INTEGER:
	    case SQL_SMALLINT:
	    case SQL_TINYINT:
	    case SQL_BIT:
	    is_integer:

		/* Pass integers through to SQL without conversion */

		rdata->bindStrings[nBound] =
		    (SQLCHAR*) ckalloc(sizeof(long));
		if (Tcl_GetLongFromObj(interp, paramValObj, 
				       (long*)(rdata->bindStrings[nBound]))
		    != TCL_OK) {
		    ckfree((char*)(rdata->bindStrings[nBound]));
		    goto is_string;
		}
		dataType = SQL_C_LONG;
		paramExternalLen = sizeof(long);
		rdata->bindStringLengths[nBound] = paramExternalLen;
		break;

	    default:
	    is_string:

		/* Everything else is converted as a string */

		if (cdata->flags & CONNECTION_FLAG_HAS_WVARCHAR) {
		    
		    /* We prefer to transfer strings in Unicode if possible */
		    
		    dataType = SQL_C_WCHAR;
		    rdata->bindStrings[nBound] = (SQLCHAR*)
			GetWCharStringFromObj(paramValObj, &paramLen);
		    rdata->bindStringLengths[nBound] =
			paramLen * sizeof(SQLWCHAR);
		    
		} else {
		    
		    /* 
		     * We need to convert the character string to system 
		     * encoding and store in rdata->bindStrings[nBound].
		     */
		    dataType = SQL_C_CHAR;
		    paramVal = Tcl_GetStringFromObj(paramValObj, &paramLen);
		    Tcl_DStringInit(&paramExternal);
		    Tcl_UtfToExternalDString(NULL, paramVal, paramLen,
					     &paramExternal);
		    paramExternalLen = Tcl_DStringLength(&paramExternal);
		    rdata->bindStrings[nBound] = (SQLCHAR*)
			ckalloc(paramExternalLen + 1);
		    memcpy(rdata->bindStrings[nBound],
			   Tcl_DStringValue(&paramExternal),
			   paramExternalLen + 1);
		    rdata->bindStringLengths[nBound] = paramExternalLen;
		    Tcl_DStringFree(&paramExternal);
		}

	    }
		
	} else {

	    /* Parameter is NULL */

	    dataType = SQL_C_CHAR;
	    rdata->bindStrings[nBound] = NULL;
	    paramExternalLen = paramLen = 0;
	    rdata->bindStringLengths[nBound] = SQL_NULL_DATA;
	}
	rc = SQLBindParameter(rdata->hStmt,
			      nBound + 1,
			      SQL_PARAM_INPUT, /* TODO - Fix this! */
			      dataType,
			      sdata->params[nBound].dataType,
			      sdata->params[nBound].precision,
			      sdata->params[nBound].scale,
			      rdata->bindStrings[nBound],
			      paramExternalLen,
			      rdata->bindStringLengths + nBound);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	    char* info = ckalloc(80 * strlen(paramName));
	    sprintf(info, "(binding the '%s' parameter)", paramName);
	    TransferSQLError(interp, SQL_HANDLE_STMT, rdata->hStmt, info);
	    ckfree(info);
	    return TCL_ERROR;
	}
    }

    /* Execute the statement */

    if (sdata->flags & STATEMENT_FLAG_TABLES) {
	rc = SQLTablesW(rdata->hStmt, NULL, 0, NULL, 0,
			sdata->nativeSqlW, sdata->nativeSqlLen, NULL, 0);
    } else if (sdata->flags & STATEMENT_FLAG_COLUMNS) {
	rc = SQLColumnsW(rdata->hStmt, NULL, 0, NULL, 0,
			 sdata->nativeSqlW, sdata->nativeSqlLen,
			 sdata->nativeMatchPatternW, sdata->nativeMatchPatLen);
    } else if (sdata->flags & STATEMENT_FLAG_TYPES) {
	rc = SQLGetTypeInfo(rdata->hStmt, sdata->typeNum);
    } else {
	rc = SQLExecute(rdata->hStmt);
    }
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO
	&& rc != SQL_NO_DATA) {
	TransferSQLError(interp, SQL_HANDLE_STMT, rdata->hStmt,
			 "(executing the statement)");
	return TCL_ERROR;
    }

    /* 
     * Extract the column information for the result set, unless we
     * already know it.
     */

    if (!(sdata->flags & STATEMENT_FLAG_RESULTS_KNOWN)) {
	if (GetResultSetDescription(interp, sdata, rdata->hStmt) != TCL_OK) {
	    return TCL_ERROR;
	}
    }

    /* Determine and store the row count */

    rc = SQLRowCount(rdata->hStmt, &(rdata->rowCount));
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	TransferSQLError(interp, SQL_HANDLE_STMT, rdata->hStmt,
			 "(counting rows in the result)");
	return TCL_ERROR;
    }
    
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ResultSetColumnsMethod --
 *
 *	Retrieves the list of columns from a result set.
 *
 * Usage:
 *	$resultSet columns
 *
 * Results:
 *	Returns the count of columns
 *
 *-----------------------------------------------------------------------------
 */

static int
ResultSetColumnsMethod(
    ClientData clientData,	/* Not used */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context,	/* Object context  */
    int objc, 			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {

    Tcl_Object thisObject = Tcl_ObjectContextObject(context);
				/* The current result set object */
    ResultSetData* rdata = (ResultSetData*)
	Tcl_ObjectGetMetadata(thisObject, &resultSetDataType);
    StatementData* sdata = (StatementData*) rdata->sdata;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 2, objv, "");
	return TCL_ERROR;
    }

    /* 
     * Extract the column information for the result set, unless we
     * already know it.
     */

    if (!(sdata->flags & STATEMENT_FLAG_RESULTS_KNOWN)) {
	if (GetResultSetDescription(interp, sdata, rdata->hStmt) != TCL_OK) {
	    return TCL_ERROR;
	}
    }

    Tcl_SetObjResult(interp, sdata->resultColNames);
    return TCL_OK;

}

/*
 *-----------------------------------------------------------------------------
 *
 * ResultSetNextrowMethod --
 *
 *	Retrieves the next row from a result set.
 *
 * Usage:
 *	$resultSet nextrow ?-as lists|dicts? ?--? variableName
 *
 * Options:
 *	-as	Selects the desired form for returning the results.
 *
 * Parameters:
 *	variableName -- Variable in which the results are to be returned
 *
 * Results:
 *	Returns a standard Tcl result.  The interpreter result is 1 if there
 *	are more rows remaining, and 0 if no more rows remain.
 *
 * Side effects:
 *	Stores in the given variable either a list or a dictionary
 *	containing one row of the result set.
 *
 *-----------------------------------------------------------------------------
 */

static int
ResultSetNextrowMethod(
    ClientData clientData,	/* Not used */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context,	/* Object context  */
    int objc, 			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {

    int lists = (int) clientData;
				/* Flag == 1 if lists are to be returned,
				 * 0 if dicts are to be returned */

    Tcl_Object thisObject = Tcl_ObjectContextObject(context);
				/* The current result set object */
    ResultSetData* rdata = (ResultSetData*)
	Tcl_ObjectGetMetadata(thisObject, &resultSetDataType);
				/* Data pertaining to the current result set */
    StatementData* sdata = (StatementData*) rdata->sdata;
				/* Statement that yielded the result set */
    ConnectionData* cdata = (ConnectionData*) sdata->cdata;
				/* Connection that opened the statement */
    PerInterpData* pidata = (PerInterpData*) cdata->pidata;
				/* Per interpreter data */
    Tcl_Obj** literals = pidata->literals;
				/* Literal pool */

    int nColumns;		/* Number of columns in the result set */
    Tcl_Obj* colName;		/* Name of the current column */
    Tcl_Obj* resultRow;		/* Row of the result set under construction */
    
    Tcl_Obj* colObj;		/* Column obtained from the row */
    SQLRETURN rc;		/* Return code from ODBC operations */
    int status = TCL_ERROR;	/* Status return from this command */

    int i;

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 2, objv, "varName");
	return TCL_ERROR;
    }

    /* 
     * Extract the column information for the result set, unless we
     * already know it.
     */

    if (!(sdata->flags & STATEMENT_FLAG_RESULTS_KNOWN)) {
	if (GetResultSetDescription(interp, sdata, rdata->hStmt) != TCL_OK) {
	    return TCL_ERROR;
	}
    }
    Tcl_ListObjLength(NULL, sdata->resultColNames, &nColumns);
    if (nColumns == 0) {
	Tcl_SetObjResult(interp, literals[LIT_0]);
	return TCL_OK;
    }

    /* Advance to the next row of the result set */

    rc = SQLFetch(rdata->hStmt);
    if (rc == SQL_NO_DATA) {
	Tcl_SetObjResult(interp, literals[LIT_0]);
	return TCL_OK;
    } else if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	TransferSQLError(interp, SQL_HANDLE_STMT, rdata->hStmt,
			 "(fetching the next row of the result set)");
	return TCL_ERROR;
    }

    /* Walk through the current row, storing data for each column */

    resultRow = Tcl_NewObj();
    Tcl_IncrRefCount(resultRow);
    for (i = 0; i < nColumns; ++i) {
	if (GetCell(rdata, interp, i, &colObj) != TCL_OK) {
	    goto cleanup;
	}

	if (lists) {
	    if (colObj == NULL) {
		colObj = Tcl_NewObj();
	    }
	    Tcl_ListObjAppendElement(NULL, resultRow, colObj);
	} else {
	    if (colObj != NULL) {
		Tcl_ListObjIndex(NULL, sdata->resultColNames, i, &colName);
		Tcl_DictObjPut(NULL, resultRow, colName, colObj);
	    }
	}
    }

    /* Save the row in the given variable */

    if (Tcl_SetVar2Ex(interp, Tcl_GetString(objv[2]), NULL,
		      resultRow, TCL_LEAVE_ERR_MSG) == NULL) {
	goto cleanup;
    }

    Tcl_SetObjResult(interp, literals[LIT_1]);
    status = TCL_OK;

 cleanup:
    Tcl_DecrRefCount(resultRow);
    return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * GetCell --
 *
 *	Service procedure to retrieve a single column in a row of a result
 *	set.
 *
 * Results:
 *	Returns a standard Tcl result.
 *
 * Side effects:
 *	If the result is TCL_OK, the column value is stored in *colObjPtr,
 *	with a zero refcount. (If the column value is NULL, NULL is stored.)
 *	If the result is TCL_ERROR, *colObjPtr is left alone, but an error
 *	message is stored in the interpreter result.
 *
 *-----------------------------------------------------------------------------
 */

static int
GetCell(
    ResultSetData* rdata,	/* Instance data for the result set */
    Tcl_Interp* interp,		/* Tcl interpreter */
    int i,			/* Column position */
    Tcl_Obj** colObjPtr		/* Returned: Tcl_Obj containing the content 
				 * or NULL */
) {

    StatementData* sdata = rdata->sdata;
    ConnectionData* cdata = sdata->cdata;
    SQLSMALLINT dataType;	/* Type of character data to retrieve */
    SQLWCHAR colWBuf[256];	/* Buffer to hold the string value of a 
				 * column */
    SQLCHAR* colBuf = (SQLCHAR*) colWBuf;
    SQLCHAR* colPtr = colBuf;	/* Pointer to the current allocated buffer
				 * (which may have grown) */
    SQLLEN colAllocLen = 256 * sizeof(SQLWCHAR);
				/* Current allocated size of the buffer,
				 * in bytes */
    SQLLEN colLen;		/* Actual size of the return value, in bytes */
    SQLINTEGER colLong;		/* Integer value of the column */
    SQLBIGINT colWide;		/* Wide-integer value of the column */
    SQLDOUBLE colDouble;	/* Double value of the column */
    Tcl_DString colDS;		/* Column expressed as a Tcl_DString */
    Tcl_Obj* colObj;		/* Column expressed as a Tcl_Obj */
    SQLRETURN rc;		/* ODBC result code */
    int retry;			/* Flag that the last ODBC operation should
				 * be retried */

    colObj = NULL;
    *colObjPtr = NULL;
    switch(sdata->results[i].dataType) {
	/* TODO: Need to return binary data as byte arrays. */
	
    case SQL_NUMERIC:
    case SQL_DECIMAL:
	
	/* 
	 * A generic 'numeric' type may fit in an int, wide,
	 * or double, and gets converted specially if it does.
	 */
	
	if (sdata->results[i].scale == 0) {
	    if (sdata->results[i].precision < 10) {
		goto convertLong;
	    } else if (sdata->results[i].precision < 19) {
		goto convertWide;
	    } else {
		/*
		 * It is tempting to convert wider integers as bignums,
		 * but Tcl does not yet export its copy of libtommath
		 * into the public API.
		 */
		goto convertString;
	    }
	} else if (sdata->results[i].precision <= 15) {
	    goto convertDouble;
	} else {
	    goto convertString;
	}
	
    case SQL_BIGINT:
    convertWide:
	/* A wide integer */
	rc = SQLGetData(rdata->hStmt, i+1, SQL_C_SBIGINT,
			(SQLPOINTER) &colWide, sizeof(colWide), &colLen);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	    char info[80];
	    sprintf(info, "(retrieving result set column #%d)\n", i+1);
	    TransferSQLError(interp, SQL_HANDLE_STMT, rdata->hStmt, info);
	    ckfree(info);
	    return TCL_ERROR;
	}
	if (colLen != SQL_NULL_DATA && colLen != SQL_NO_TOTAL) {
	    colObj = Tcl_NewWideIntObj((Tcl_WideInt)colWide);
	}
	break;
	
    case SQL_BIT:
    case SQL_INTEGER:
    case SQL_SMALLINT:
    case SQL_TINYINT:
    convertLong:
	/* An integer no larger than 'long' */
	rc = SQLGetData(rdata->hStmt, i+1, SQL_C_SLONG,
			(SQLPOINTER) &colLong, sizeof(colLong), &colLen);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	    char info[80];
	    sprintf(info, "(retrieving result set column #%d)\n", i+1);
	    TransferSQLError(interp, SQL_HANDLE_STMT, rdata->hStmt, info);
	    ckfree(info);
	    return TCL_ERROR;
	}
	if (colLen != SQL_NULL_DATA && colLen != SQL_NO_TOTAL) {
	    colObj = Tcl_NewLongObj(colLong);
	}
	break;
	
    case SQL_FLOAT:
	/*
	 * A 'float' is converted to a 'double' if it fits;
	 * to a string, otherwise.
	 */
	if (sdata->results[i].precision <= 53) {
	    goto convertDouble;
	} else {
	    goto convertString;
	}
	
    case SQL_REAL:
    case SQL_DOUBLE:
    convertDouble:
	/*
	 * A single- or double-precision floating point number.
	 * Reals are widened to doubles.
	 */
	rc = SQLGetData(rdata->hStmt, i+1, SQL_C_DOUBLE,
			(SQLPOINTER) &colDouble, sizeof(colDouble),
			&colLen);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	    char info[80];
	    sprintf(info, "(retrieving result set column #%d)\n", i+1);
	    TransferSQLError(interp, SQL_HANDLE_STMT, rdata->hStmt, info);
	    ckfree(info);
	    return TCL_ERROR;
	}
	if (colLen != SQL_NULL_DATA && colLen != SQL_NO_TOTAL) {
	    colObj = Tcl_NewDoubleObj(colDouble);
	}
	break;
	
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
	dataType = SQL_C_CHAR;
	goto convertString;

    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
	dataType = SQL_C_WCHAR;
	goto convertString;

    default:
	if (cdata->flags & CONNECTION_FLAG_HAS_WVARCHAR) {
	    dataType = SQL_C_WCHAR;
	} else {
	    dataType = SQL_C_CHAR;
	}
	goto convertString;

    convertString:
	/* Anything else is converted as a string */
	retry = 0;
	do {
	    /* 
	     * It's possible that SQLGetData won't update colLen if
	     * SQL_ERROR is returned. Store a background of zero so
	     * that it's always initialized.
	     */
	    colLen = 0;
	    rc = SQLGetData(rdata->hStmt, i+1, dataType,
			    (SQLPOINTER) colPtr, colAllocLen,
			    &colLen);
	    if (colLen >= colAllocLen) {
		colAllocLen = 2 * colLen + sizeof(SQLWCHAR);
		if (colPtr != colBuf) {
		    ckfree((char*) colPtr);
		}
		colPtr = (SQLCHAR*)
		    ckalloc(colAllocLen);
		retry = 1;
	    }
	} while (retry);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
	    char info[80];		
	    sprintf(info, "(retrieving result set column #%d)\n", i+1);
	    TransferSQLError(interp, SQL_HANDLE_STMT, rdata->hStmt, info);
	    if (colPtr != colBuf) {
		ckfree((char*) colPtr);
	    }
	    return TCL_ERROR;
	}
	if (colLen >= 0) {
	    Tcl_DStringInit(&colDS);
	    if (dataType == SQL_C_CHAR) {
		Tcl_ExternalToUtfDString(NULL, (char*) colPtr, (int)colLen,
					 &colDS);
	    } else {
		DStringAppendWChars(&colDS, (SQLWCHAR*) colPtr,
				    (int)(colLen / sizeof(SQLWCHAR)));
	    }
	    colObj = Tcl_NewStringObj(Tcl_DStringValue(&colDS),
				      Tcl_DStringLength(&colDS));
	    Tcl_DStringFree(&colDS);
	}
	break;
	
    } /* end of switch */

    *colObjPtr = colObj;
    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * ResultSetRowcountMethod --
 *
 *	Returns (if known) the number of rows affected by an ODBC statement.
 *
 * Usage:
 *	$resultSet rowcount
 *
 * Results:
 *	Returns a standard Tcl result giving the number of affected rows.
 *
 *-----------------------------------------------------------------------------
 */

static int
ResultSetRowcountMethod(
    ClientData clientData,	/* Not used */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context,	/* Object context  */
    int objc, 			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {

    Tcl_Object thisObject = Tcl_ObjectContextObject(context);
				/* The current result set object */
    ResultSetData* rdata = (ResultSetData*)
	Tcl_ObjectGetMetadata(thisObject, &resultSetDataType);
				/* Data pertaining to the current result set */

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 2, objv, "");
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewLongObj(rdata->rowCount));
    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * DeleteResultSetMetadata, DeleteResultSet --
 *
 *	Cleans up when an ODBC result set is no longer required.
 *
 * Side effects:
 *	Frees all resources associated with the result set.
 *
 *-----------------------------------------------------------------------------
 */

static void
DeleteResultSetMetadata(
    ClientData clientData	/* Instance data for the connection */
) {
    DecrResultSetRefCount((ResultSetData*)clientData);
}
static void
DeleteResultSet(
    ResultSetData* rdata	/* Metadata for the result set */
) {
    StatementData* sdata = rdata->sdata;
    FreeBoundParameters(rdata);
    if (rdata->hStmt != NULL) {
	if (rdata->hStmt != sdata->hStmt) {
	    SQLFreeHandle(SQL_HANDLE_STMT, rdata->hStmt);
	} else {
	    SQLCloseCursor(rdata->hStmt);
	    sdata->flags &= ~STATEMENT_FLAG_HSTMT_BUSY;
	}
    }
    DecrStatementRefCount(rdata->sdata);
    ckfree((char*)rdata);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CloneResultSet --
 *
 *	Attempts to clone an ODBC result set's metadata.
 *
 * Results:
 *	Returns the new metadata
 *
 * At present, we don't attempt to clone result sets - it's not obvious
 * that such an action would ever even make sense.  Instead, we throw an
 * error.
 *
 *-----------------------------------------------------------------------------
 */

static int
CloneResultSet(
    Tcl_Interp* interp,		/* Tcl interpreter for error reporting */
    ClientData metadata,	/* Metadata to be cloned */
    ClientData* newMetaData	/* Where to put the cloned metadata */
) {
    Tcl_SetObjResult(interp,
		     Tcl_NewStringObj("ODBC result sets are not clonable", -1));
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * FreeBoundParameters --
 *
 *	Frees the bound parameters in a result set after it has been executed
 *	or when an error prevents its execution
 *
 *-----------------------------------------------------------------------------
 */

static void
FreeBoundParameters(
    ResultSetData* rdata	/* Result set being abandoned */
) {
    int nParams;
    int i;
    if (rdata->bindStrings != NULL) {
	Tcl_ListObjLength(NULL, rdata->sdata->subVars, &nParams);
	for (i = 0; i < nParams; ++i) {
	    if (rdata->bindStrings[i] != NULL) {
		ckfree((char*) rdata->bindStrings[i]);
	    }
	}
	ckfree((char*) rdata->bindStrings);
	ckfree((char*) rdata->bindStringLengths);
	rdata->bindStrings = NULL;
    }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Datasources_ObjCmd --
 *
 *	Enumerates the ODBC data sources.
 *
 * Usage:
 *
 *	tdbc::odbc::datasources ?-system | -user?
 *
 * Results:
 *	Returns a dictionary whose keys are the names of data sources and
 *	whose values are data source descriptions.
 *
 * The -system flag restricts the data sources to system data sources; 
 * the -user flag to user data sources. If no flag is specified, both types
 * are returned.
 *
 *-----------------------------------------------------------------------------
 */

static int
DatasourcesObjCmd(
    ClientData clientData,	/* Opaque pointer to per-interp data */
    Tcl_Interp* interp,		/* Tcl interpreter */
    int objc,			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    PerInterpData* pidata = (PerInterpData*) clientData;
    SQLSMALLINT initDirection = SQL_FETCH_FIRST;
    SQLSMALLINT direction;
    struct flag {
	const char* name;
	SQLSMALLINT value;
    } flags[] = {
	{ "-system", SQL_FETCH_FIRST_SYSTEM },
	{ "-user", SQL_FETCH_FIRST_USER },
	{ NULL, 0 }
    };
    int flagIndex;
    SQLRETURN rc;		/* SQL result code */
    SQLWCHAR serverName[SQL_MAX_DSN_LENGTH + 1];
				/* Data source name */
    SQLSMALLINT serverNameLen;	/* Length of the DSN */
    SQLWCHAR *description;	/* Data source descroption */
    SQLSMALLINT descLen;	/* Length of the description */
    SQLSMALLINT descAllocLen;	/* Allocated size of the description */
    SQLSMALLINT descLenNeeded;	/* Length needed for the description */
    Tcl_Obj* retval;		/* Return value */
    Tcl_DString nameDS;		/* Buffer for a name or description */
    Tcl_Obj* nameObj;		/* Name or description as a Tcl object */
    int finished;		/* Flag == 1 if a complete list of data
				 * sources has been constructed */
    int status = TCL_OK;	/* Status return from this call */

    /* Get the argument */

    if (objc > 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "?-system|-user?");
	return TCL_ERROR;
    }
    if (objc == 2) {
	if (Tcl_GetIndexFromObjStruct(interp, objv[1], (const void*) flags,
				      sizeof(struct flag),
				      "option", 0, &flagIndex) != TCL_OK) {
	    return TCL_ERROR;
	}
	initDirection = flags[flagIndex].value;
    }

    /* Allocate memory */

    retval = Tcl_NewObj();
    Tcl_IncrRefCount(retval);
    descLenNeeded = 32;
    finished = 0;

    while (!finished) {

	direction = initDirection;
	finished = 1;
	descAllocLen = descLenNeeded;
	description = (SQLWCHAR*)
	    ckalloc(sizeof(SQLWCHAR) * (descAllocLen + 1));
	Tcl_SetListObj(retval, 0, NULL);

	/* Enumerate the data sources */

	while (1) {
	    rc = SQLDataSourcesW(pidata->hEnv, direction, serverName,
				 SQL_MAX_DSN_LENGTH + 1, &serverNameLen,
				 description, descAllocLen, &descLen);
	    direction = SQL_FETCH_NEXT;
	    
	    if (descLen > descLenNeeded) {

		/* The description buffer wasn't big enough. */
		
		descLenNeeded = 2 * descLen;
		finished = 0;
		break;

	    } else if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
		
		/* Got a data source; add key and value to the dictionary */
		
		Tcl_DStringInit(&nameDS);
		DStringAppendWChars(&nameDS, serverName, serverNameLen);
		nameObj = Tcl_NewStringObj(Tcl_DStringValue(&nameDS),
					   Tcl_DStringLength(&nameDS));
		Tcl_ListObjAppendElement(NULL, retval, nameObj);
		Tcl_DStringFree(&nameDS);
		
		Tcl_DStringInit(&nameDS);
		DStringAppendWChars(&nameDS, description, descLen);
		nameObj = Tcl_NewStringObj(Tcl_DStringValue(&nameDS),
					   Tcl_DStringLength(&nameDS));
		Tcl_ListObjAppendElement(NULL, retval, nameObj);
		Tcl_DStringFree(&nameDS);
		
	    } else if (rc == SQL_NO_DATA) {

		/* End of data sources */
		
		if (finished) {
		    Tcl_SetObjResult(interp, retval);
		    status = TCL_OK;
		}
		break;
		
	    } else {
		
		/* Anything else is an error */
		
		TransferSQLError(interp, SQL_HANDLE_ENV, pidata->hEnv, 
				 "(retrieving data source names)");
		status = TCL_ERROR;
		finished = 1;
		break;
	    }
	}

	ckfree((char*) description);
    }
    Tcl_DecrRefCount(retval);

    return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Drivers_ObjCmd --
 *
 *	Enumerates the ODBC drivers.
 *
 * Usage:
 *
 *	tdbc::odbc::drivers
 *
 * Results:
 *	Returns a dictionary whose keys are the names of drivers and
 *	whose values are lists of attributes
 *
 *-----------------------------------------------------------------------------
 */

static int
DriversObjCmd(
    ClientData clientData,	/* Opaque pointer to per-interp data */
    Tcl_Interp* interp,		/* Tcl interpreter */
    int objc,			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    PerInterpData* pidata = (PerInterpData*) clientData;
    SQLSMALLINT direction;
    SQLRETURN rc;		/* SQL result code */
    SQLWCHAR *driver;		/* Driver name */
    SQLSMALLINT driverLen;	/* Length of the driver name */
    SQLSMALLINT driverAllocLen; /* Allocated size of the driver name */
    SQLSMALLINT driverLenNeeded; /* Required size of the driver name */
    SQLWCHAR *attributes;	/* Driver attributes */
    SQLSMALLINT attrLen;	/* Length of the driver attributes */
    SQLSMALLINT attrAllocLen;	/* Allocated size of the driver attributes */
    SQLSMALLINT attrLenNeeded;	/* Length needed for the driver attributes */
    Tcl_Obj* retval;		/* Return value */
    Tcl_Obj* attrObj;		/* Tcl object to hold driver attribute list */
    Tcl_DString nameDS;		/* Buffer for a name or attribute */
    Tcl_Obj* nameObj;		/* Name or attribute as a Tcl object */
    int finished;		/* Flag == 1 if a complete list of drivers
				 * has been constructed */
    int status = TCL_OK;	/* Status return from this call */
    int i, j;

    /* Get the argument */

    if (objc > 1) {
	Tcl_WrongNumArgs(interp, 1, objv, "");
	return TCL_ERROR;
    }

    /* Allocate memory */

    retval = Tcl_NewObj();
    Tcl_IncrRefCount(retval);
    driverLenNeeded = 32;
    attrLenNeeded = 32;
    finished = 0;

    while (!finished) {

	finished = 1;
	driverAllocLen = driverLenNeeded;
	driver = (SQLWCHAR*)
	    ckalloc(sizeof(SQLWCHAR) * (driverAllocLen + 1));
	attrAllocLen = attrLenNeeded;
	attributes = (SQLWCHAR*)
	    ckalloc(sizeof(SQLWCHAR) * (attrAllocLen + 1));
	Tcl_SetListObj(retval, 0, NULL);
	direction = SQL_FETCH_FIRST;

	/* Enumerate the data sources */

	while (1) {
	    rc = SQLDriversW(pidata->hEnv, direction, driver,
			     driverAllocLen, &driverLen,
			     attributes, attrAllocLen, &attrLen);
	    direction = SQL_FETCH_NEXT;
	    
	    if (driverLen > driverLenNeeded) {

		/* The description buffer wasn't big enough. */
		
		driverLenNeeded = 2 * driverLen;
		finished = 0;
		break;
	    }
	    if (attrLen > attrLenNeeded) {

		/* The attributes buffer wasn't big enough. */

		attrLenNeeded = 2 * attrLen;
		finished = 0;
		break;
	    }

	    if (finished) {
		if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
		    
		    /* Got a data source; add key and value to the dictionary */
		    
		    Tcl_DStringInit(&nameDS);
		    DStringAppendWChars(&nameDS, driver, driverLen);
		    nameObj = Tcl_NewStringObj(Tcl_DStringValue(&nameDS),
					       Tcl_DStringLength(&nameDS));
		    Tcl_ListObjAppendElement(NULL, retval, nameObj);
		    Tcl_DStringFree(&nameDS);
		    
		    /* 
		     * Attributes are a set of U+0000-terminated
		     * strings, ending with an extra U+0000
		     */
		    attrObj = Tcl_NewObj();
		    for (i = 0; attributes[i] != 0; ) {
			for (j = i; attributes[j] != 0; ++j) {
			    /* do nothing */
			}
			Tcl_DStringInit(&nameDS);
			DStringAppendWChars(&nameDS, attributes+i, j-i);
			nameObj = Tcl_NewStringObj(Tcl_DStringValue(&nameDS),
						   Tcl_DStringLength(&nameDS));
			Tcl_ListObjAppendElement(NULL, attrObj, nameObj);
			Tcl_DStringFree(&nameDS);
			i = j + 1;
		    }
		    Tcl_ListObjAppendElement(NULL, retval, attrObj);
		    
		} else if (rc == SQL_NO_DATA) {
		    
		    /* End of data sources */
		    
		    if (finished) {
			Tcl_SetObjResult(interp, retval);
			status = TCL_OK;
		    }
		    break;
		    
		} else {
		    
		    /* Anything else is an error */
		    
		    TransferSQLError(interp, SQL_HANDLE_ENV, pidata->hEnv, 
				     "(retrieving data source names)");
		    status = TCL_ERROR;
		    finished = 1;
		    break;
		}
	    }
	}
	ckfree((char*) driver);
	ckfree((char*) attributes);
    }
    Tcl_DecrRefCount(retval);

    return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Tdbcodbc_Init --
 *
 *	Initializes the TDBC-ODBC bridge when this library is loaded.
 *
 * Side effects:
 *	Creates the ::tdbc::odbc namespace and the commands that reside in it.
 *	Initializes the ODBC environment.
 *
 *-----------------------------------------------------------------------------
 */

extern TDBCODBCAPI int
Tdbcodbc_Init(
    Tcl_Interp* interp		/* Tcl interpreter */
) {
    SQLHENV hEnv;		/* ODBC environemnt */
    PerInterpData* pidata;	/* Per-interpreter data for this package */
    Tcl_Obj* nameObj;		/* Name of a class or method being looked up */
    Tcl_Object curClassObject;  /* Tcl_Object representing the current class */
    Tcl_Class curClass;		/* Tcl_Class representing the current class */
    int i;

    /* Require all package dependencies */

    if (Tcl_InitStubs(interp, TCL_VERSION, 0) == NULL) {
	return TCL_ERROR;
    }
    if (Tcl_OOInitStubs(interp) == NULL) {
	return TCL_ERROR;
    }
    if (Tdbc_InitStubs(interp) == NULL) {
	return TCL_ERROR;
    }

    /* Provide the current package */

    if (Tcl_PkgProvide(interp, "tdbc::odbc", PACKAGE_VERSION) == TCL_ERROR) {
	return TCL_ERROR;
    }

    /* Initialize the ODBC environment */

    hEnv = GetHEnv(interp);
    if (hEnv == SQL_NULL_HANDLE) {
	return TCL_ERROR;
    }

    /* 
     * Evaluate the initialization script to make the connection class 
     */

    if (Tcl_Eval(interp, initScript) != TCL_OK) {
	return TCL_ERROR;
    }

    /*
     * Create per-interpreter data for the package
     */

    pidata = (PerInterpData*) ckalloc(sizeof(PerInterpData));
    pidata->refCount = 1;
    pidata->hEnv = GetHEnv(NULL);
    for (i = 0; i < LIT__END; ++i) {
	pidata->literals[i] = Tcl_NewStringObj(LiteralValues[i], -1);
	Tcl_IncrRefCount(pidata->literals[i]);
    }

    /* 
     * Find the connection class, and attach an 'init' method to
     * it. Hold the SQLENV in the method's client data.
     */

    nameObj = Tcl_NewStringObj("::tdbc::odbc::connection", -1);
    Tcl_IncrRefCount(nameObj);
    if ((curClassObject = Tcl_GetObjectFromObj(interp, nameObj)) == NULL) {
	Tcl_DecrRefCount(nameObj);
	return TCL_ERROR;
    }
    Tcl_DecrRefCount(nameObj);
    curClass = Tcl_GetObjectAsClass(curClassObject);
    nameObj = Tcl_NewStringObj("init", -1);
    Tcl_IncrRefCount(nameObj);
    Tcl_NewMethod(interp, curClass, nameObj, 0, &ConnectionInitMethodType,
		       (ClientData) pidata);
    Tcl_DecrRefCount(nameObj);

    /* Attach the other methods to the connection class */

    nameObj = Tcl_NewStringObj("commit", -1);
    Tcl_IncrRefCount(nameObj);
    Tcl_NewMethod(interp, curClass, nameObj, 1,
		       &ConnectionEndXcnMethodType, (ClientData) SQL_COMMIT);
    Tcl_DecrRefCount(nameObj);
    nameObj = Tcl_NewStringObj("rollback", -1);
    Tcl_IncrRefCount(nameObj);
    Tcl_NewMethod(interp, curClass, nameObj, 1,
		       &ConnectionEndXcnMethodType, (ClientData) SQL_ROLLBACK);
    Tcl_DecrRefCount(nameObj);
    for (i = 0; ConnectionMethods[i] != NULL; ++i) {
	nameObj = Tcl_NewStringObj(ConnectionMethods[i]->name, -1);
	Tcl_IncrRefCount(nameObj);
	Tcl_NewMethod(interp, curClass, nameObj, 1, ConnectionMethods[i],
			   (ClientData) NULL);
	Tcl_DecrRefCount(nameObj);
    }

    /* Look up the 'statement' class */

    nameObj = Tcl_NewStringObj("::tdbc::odbc::statement", -1);
    Tcl_IncrRefCount(nameObj);
    if ((curClassObject = Tcl_GetObjectFromObj(interp, nameObj)) == NULL) {
	Tcl_DecrRefCount(nameObj);
	return TCL_ERROR;
    }
    Tcl_DecrRefCount(nameObj);
    curClass = Tcl_GetObjectAsClass(curClassObject);

    /* Attach the methods to the 'statement' class */

    for (i = 0; StatementMethods[i] != NULL; ++i) {
	nameObj = Tcl_NewStringObj(StatementMethods[i]->name, -1);
	Tcl_IncrRefCount(nameObj);
	Tcl_NewMethod(interp, curClass, nameObj, 1, StatementMethods[i],
			   (ClientData) NULL);
	Tcl_DecrRefCount(nameObj);
    }

    /* Look up the 'tablesStatement' class */

    nameObj = Tcl_NewStringObj("::tdbc::odbc::tablesStatement", -1);
    Tcl_IncrRefCount(nameObj);
    if ((curClassObject = Tcl_GetObjectFromObj(interp, nameObj)) == NULL) {
	Tcl_DecrRefCount(nameObj);
	return TCL_ERROR;
    }
    Tcl_DecrRefCount(nameObj);
    curClass = Tcl_GetObjectAsClass(curClassObject);

    /* Attach the methods to the 'tablesStatement' class */

    for (i = 0; TablesStatementMethods[i] != NULL; ++i) {
	nameObj = Tcl_NewStringObj(TablesStatementMethods[i]->name, -1);
	Tcl_IncrRefCount(nameObj);
	Tcl_NewMethod(interp, curClass, nameObj, 1,
			   TablesStatementMethods[i], (ClientData) NULL);
	Tcl_DecrRefCount(nameObj);
    }

    /* Look up the 'columnsStatement' class */

    nameObj = Tcl_NewStringObj("::tdbc::odbc::columnsStatement", -1);
    Tcl_IncrRefCount(nameObj);
    if ((curClassObject = Tcl_GetObjectFromObj(interp, nameObj)) == NULL) {
	Tcl_DecrRefCount(nameObj);
	return TCL_ERROR;
    }
    Tcl_DecrRefCount(nameObj);
    curClass = Tcl_GetObjectAsClass(curClassObject);

    /* Attach the methods to the 'columnsStatement' class */

    for (i = 0; ColumnsStatementMethods[i] != NULL; ++i) {
	nameObj = Tcl_NewStringObj(ColumnsStatementMethods[i]->name, -1);
	Tcl_IncrRefCount(nameObj);
	Tcl_NewMethod(interp, curClass, nameObj, 1,
			   ColumnsStatementMethods[i], (ClientData) NULL);
	Tcl_DecrRefCount(nameObj);
    }

    /* Look up the 'typesStatement' class */

    nameObj = Tcl_NewStringObj("::tdbc::odbc::typesStatement", -1);
    Tcl_IncrRefCount(nameObj);
    if ((curClassObject = Tcl_GetObjectFromObj(interp, nameObj)) == NULL) {
	Tcl_DecrRefCount(nameObj);
	return TCL_ERROR;
    }
    Tcl_DecrRefCount(nameObj);
    curClass = Tcl_GetObjectAsClass(curClassObject);

    /* Attach the methods to the 'typesStatement' class */

    for (i = 0; TypesStatementMethods[i] != NULL; ++i) {
	nameObj = Tcl_NewStringObj(TypesStatementMethods[i]->name, -1);
	Tcl_IncrRefCount(nameObj);
	Tcl_NewMethod(interp, curClass, nameObj, 1,
			   TypesStatementMethods[i], (ClientData) NULL);
	Tcl_DecrRefCount(nameObj);
    }

    /* Look up the 'resultSet' class */

    nameObj = Tcl_NewStringObj("::tdbc::odbc::resultset", -1);
    Tcl_IncrRefCount(nameObj);
    if ((curClassObject = Tcl_GetObjectFromObj(interp, nameObj)) == NULL) {
	Tcl_DecrRefCount(nameObj);
	return TCL_ERROR;
    }
    Tcl_DecrRefCount(nameObj);
    curClass = Tcl_GetObjectAsClass(curClassObject);

    /* Attach the methods to the 'resultSet' class */

    for (i = 0; ResultSetMethods[i] != NULL; ++i) {
	nameObj = Tcl_NewStringObj(ResultSetMethods[i]->name, -1);
	Tcl_IncrRefCount(nameObj);
	Tcl_NewMethod(interp, curClass, nameObj, 1, ResultSetMethods[i],
			   (ClientData) NULL);
	Tcl_DecrRefCount(nameObj);
    }
    nameObj = Tcl_NewStringObj("nextlist", -1);
    Tcl_IncrRefCount(nameObj);
    Tcl_NewMethod(interp, curClass, nameObj, 1, &ResultSetNextrowMethodType,
		  (ClientData) 1);
    Tcl_DecrRefCount(nameObj);
    nameObj = Tcl_NewStringObj("nextdict", -1);
    Tcl_IncrRefCount(nameObj);
    Tcl_NewMethod(interp, curClass, nameObj, 1, &ResultSetNextrowMethodType,
		  (ClientData) 0);
    Tcl_DecrRefCount(nameObj);

    IncrPerInterpRefCount(pidata);
    Tcl_CreateObjCommand(interp, "tdbc::odbc::datasources",
			 DatasourcesObjCmd, (ClientData) pidata, DeleteCmd);
    IncrPerInterpRefCount(pidata);
    Tcl_CreateObjCommand(interp, "tdbc::odbc::drivers",
			 DriversObjCmd, (ClientData) pidata, DeleteCmd);


    DismissHEnv();
    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * DeletePerInterpData --
 *
 *	Delete per-interpreter data when the ODBC package is finalized
 *
 * Side effects:
 *	Releases the (presumably last) reference on the environment handle,
 *	cleans up the literal pool, and deletes the per-interp data structure.
 *
 *-----------------------------------------------------------------------------
 */

static void
DeletePerInterpData(
    PerInterpData* pidata	/* Data structure to clean up */
) {
    int i;
    DismissHEnv();
    for (i = 0; i < LIT__END; ++i) {
	Tcl_DecrRefCount(pidata->literals[i]);
    }
    ckfree((char *) pidata);
}
