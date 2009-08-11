//TODO: use OCIEnvCreate
//

/*
 * tdbcoracle.c --
 *
 *	Bridge between TDBC (Tcl DataBase Connectivity) and ORACLE.
 *
 * Copyright (c) 2008, 2009 by Kevin B. Kenny.
 *
 * Please refer to the file, 'license.terms' for the conditions on
 * redistribution of this file and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * $Id: $
 *
 *-----------------------------------------------------------------------------
 */

#include <tcl.h>
#include <tclOO.h>
#include <tdbc.h>

#include <stdio.h>
#include <string.h>

#include <oci.h>

#define not_imp { printf("%s: not implemented yet\n", __FUNCTION__); fflush(stdout); return TCL_ERROR;}

/*
 * Objects to create within the literal pool
 */

const char* LiteralValues[] = {
    "",
    "0",
    "1",
    "direction",
    "in",
    "inout",
    "name",
    "nullable",
    "out",
    "precision",
    "scale",
    "type",
    NULL
};
enum LiteralIndex {
    LIT_EMPTY,
    LIT_0,
    LIT_1,
    LIT_DIRECTION,
    LIT_IN,
    LIT_INOUT,
    LIT_NAME,
    LIT_NULLABLE,
    LIT_OUT,
    LIT_PRECISION,
    LIT_SCALE,
    LIT_TYPE,
    LIT__END
};

typedef struct PostgresDataType {
    const char* name;		/* Type name */
    int num;			/* Type number */
} PostgresDataType;
static const PostgresDataType dataTypes[] = {
    { "NULL",	    0},
    { NULL,	    0}
};


/*
 * Structure that holds per-interpreter data for the ORACLE package.
 */

typedef struct PerInterpData {
    int refCount;		/* Reference count */
    Tcl_Obj* literals[LIT__END];
				/* Literal pool */
    Tcl_HashTable typeNumHash;	/* Lookup table for type numbers */
    OCIEnv *ociEnvHp;		/* OCI environment handle */
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
 * Structure that carries the data for an ORACLE connection
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
    int flags;
    OCIError*	ociErrHp;	/* OCI Error handle */
    OCIServer*	ociSrvHp;	/* OCI Server handle */
    OCISvcCtx*	ociSvcHp;	/* OCI Service handle */
    OCISession* ociAutHp;	/* OCI Session handle */
    OCIEnv *ociEnvHp;		/* OCI environment handle (copy from pidata)*/
} ConnectionData;

/*
 * Flags for the state of an ORACLE connection
 */

//#define CONN_FLAG_AUTOCOMMIT	0x1	/* Autocommit is set */
//#define CONN_FLAG_IN_XCN	0x2 	/* Transaction is in progress */
//#define CONN_FLAG_INTERACTIVE	0x4	/* -interactive requested at connect */

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
 * Structure that carries the data for a Oracle prepared statement.
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
    struct ParamData *params;	/* Data types and attributes of parameters */
    Tcl_Obj* nativeSql;		/* Native SQL statement to pass into
				 * Oracle */
    Tcl_Obj* columnNames;	/* Column names in the result set */
    OCIStmt* ociStmtHp;		/* OCI statement Handle */ 
    int flags;
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

/* Flags in the 'StatementData->flags' word */

#define STMT_FLAG_BUSY		0x1	/* Statement handle is in use */

/*
 * Structure describing the data types of substituted parameters in
 * a SQL statement.
 */

typedef struct ParamData {
    int flags;			/* Flags regarding the parameters - see below */
    int dataType;		/* Data type */
    int precision;		/* Size of the expected data */
    int scale;			/* Digits after decimal point of the
				 * expected data */
} ParamData;

#define PARAM_KNOWN	1<<0	/* Something is known about the parameter */
#define PARAM_IN 	1<<1	/* Parameter is an input parameter */
#define PARAM_OUT 	1<<2	/* Parameter is an output parameter */
				/* (Both bits are set if parameter is
				 * an INOUT parameter) */
#define PARAM_BINARY	1<<3	/* Parameter is binary */

/*
 * Structure describing a Oracle result set.  The object that the Tcl
 * API terms a "result set" actually has to be represented by a Oracle
 * "statement", since a Oracle statement can have only one set of results
 * at any given time.
 */

typedef struct ResultSetData {
    int refCount;		/* Reference count */
    StatementData* sdata;	/* Statement that generated this result set */
    Tcl_Obj* paramValues;	/* List of parameter values */
    OCIStmt* ociStmtHp;		/* OCI statement Handle */ 
    ub2* resultLengths;		/* Length of output fields */
    char** resultBindings;	/* Array of output field values */
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

/* Table of Oracle type names */

/* Configuration options for Oracle connections */

/* Data types of configuration options */

enum OptType {
    TYPE_STRING,		/* Arbitrary character string */
};

/* Locations of the string options in the string array */

enum OptStringIndex {
    INDX_DBLINK, INDX_USER, INDX_PASS, INDX_MAX
};

/* Flags in the configuration table */

#define CONN_OPT_FLAG_MOD 0x1	/* Configuration value changable at runtime */
#define CONN_OPT_FLAG_ALIAS 0x2	/* Configuration option is an alias */

 /* Table of configuration options */

static const struct {
    const char * name;	/* Option name */
    enum OptType type;	/* Option data type */
    int info;		/* Option index or flag value */
    int flags;		/* Flags - modifiable; SSL related; is an alias */
    const char* query;	/* How to determine the option value? */
} ConnOptions [] = {
    { "-dblink",    TYPE_STRING,    INDX_DBLINK,    0,			    NULL},
    { "-db",	    TYPE_STRING,    INDX_DBLINK,    CONN_OPT_FLAG_ALIAS,    NULL},
    { "-user",	    TYPE_STRING,    0,		    CONN_OPT_FLAG_ALIAS,    NULL},
    { "-user",	    TYPE_STRING,    0,		    CONN_OPT_FLAG_ALIAS,    NULL},
    { NULL,	    0,		    0,		    0,			    NULL}
};

/* Declarations of static functions appearing in this file */

static int TransferOracleError(Tcl_Interp* interp, OCIError* ociErrHp,	sword status);
//static void TransferOracleStmtError(Tcl_Interp* interp, ORACLE_STMT* oraclePtr);

static Tcl_Obj* QueryConnectionOption(ConnectionData* cdata, Tcl_Interp* interp,
				      int optionNum);
static int ConfigureConnection(PerInterpData* pidata, ConnectionData* cdata, Tcl_Interp* interp,
			       int objc, Tcl_Obj *const objv[], int skip);
static int ConnectionConstructor(ClientData clientData, Tcl_Interp* interp,
				 Tcl_ObjectContext context,
				 int objc, Tcl_Obj *const objv[]);
//static int ConnectionBegintransactionMethod(ClientData clientData,
//					    Tcl_Interp* interp,
//					    Tcl_ObjectContext context,
//					    int objc, Tcl_Obj *const objv[]);
//static int ConnectionColumnsMethod(ClientData clientData, Tcl_Interp* interp,
//				  Tcl_ObjectContext context,
//				  int objc, Tcl_Obj *const objv[]);
//static int ConnectionCommitMethod(ClientData clientData, Tcl_Interp* interp,
//				  Tcl_ObjectContext context,
//				  int objc, Tcl_Obj *const objv[]);
//static int ConnectionConfigureMethod(ClientData clientData, Tcl_Interp* interp,
//				     Tcl_ObjectContext context,
//				     int objc, Tcl_Obj *const objv[]);
//static int ConnectionRollbackMethod(ClientData clientData, Tcl_Interp* interp,
//				    Tcl_ObjectContext context,
//				    int objc, Tcl_Obj *const objv[]);
//static int ConnectionSetCollationInfoMethod(ClientData clientData,
//					    Tcl_Interp* interp,
//					    Tcl_ObjectContext context,
//					    int objc, Tcl_Obj *const objv[]);
//static int ConnectionTablesMethod(ClientData clientData, Tcl_Interp* interp,
//				  Tcl_ObjectContext context,
//				  int objc, Tcl_Obj *const objv[]);

static void DeleteConnectionMetadata(ClientData clientData);
static void DeleteConnection(ConnectionData* cdata);
static int CloneConnection(Tcl_Interp* interp, ClientData oldClientData,
			   ClientData* newClientData);

static StatementData* NewStatement(ConnectionData* cdata);
static OCIStmt* AllocAndPrepareStatement(Tcl_Interp* interp,
					    StatementData* sdata);

static Tcl_Obj* ResultDescToTcl(ResultSetData* rdata, int flags);

static int StatementConstructor(ClientData clientData, Tcl_Interp* interp,
				Tcl_ObjectContext context,
				int objc, Tcl_Obj *const objv[]);
//static int StatementParamtypeMethod(ClientData clientData, Tcl_Interp* interp,
//				    Tcl_ObjectContext context,
//				    int objc, Tcl_Obj *const objv[]);
//static int StatementParamsMethod(ClientData clientData, Tcl_Interp* interp,
//				 Tcl_ObjectContext context,
//				 int objc, Tcl_Obj *const objv[]);

static void DeleteStatementMetadata(ClientData clientData);
static void DeleteStatement(StatementData* sdata);
static int CloneStatement(Tcl_Interp* interp, ClientData oldClientData,
			  ClientData* newClientData);

static int ResultSetConstructor(ClientData clientData, Tcl_Interp* interp,
				Tcl_ObjectContext context,
				int objc, Tcl_Obj *const objv[]);
static int ResultSetColumnsMethod(ClientData clientData, Tcl_Interp* interp,
				  Tcl_ObjectContext context,
				  int objc, Tcl_Obj *const objv[]);
static int ResultSetNextrowMethod(ClientData clientData, Tcl_Interp* interp,
				  Tcl_ObjectContext context,
				  int objc, Tcl_Obj *const objv[]);
//static int ResultSetRowcountMethod(ClientData clientData, Tcl_Interp* interp,
//				   Tcl_ObjectContext context,
//				   int objc, Tcl_Obj *const objv[]);

static void DeleteResultSetMetadata(ClientData clientData);
static void DeleteResultSet(ResultSetData* rdata);
static int CloneResultSet(Tcl_Interp* interp, ClientData oldClientData,
			  ClientData* newClientData);


static void DeleteCmd(ClientData clientData);
static int CloneCmd(Tcl_Interp* interp,
		    ClientData oldMetadata, ClientData* newMetadata);

static void DeletePerInterpData(PerInterpData* pidata);

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

const static Tcl_MethodType ConnectionConstructorType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "CONSTRUCTOR",		/* name */
    ConnectionConstructor,	/* callProc */
    DeleteCmd,			/* deleteProc */
    CloneCmd			/* cloneProc */
};

#if 0
const static Tcl_MethodType ConnectionBegintransactionMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "begintransaction",		/* name */
    ConnectionBegintransactionMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};
const static Tcl_MethodType ConnectionColumnsMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "Columns",			/* name */
    ConnectionColumnsMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};
const static Tcl_MethodType ConnectionCommitMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "commit",			/* name */
    ConnectionCommitMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};
const static Tcl_MethodType ConnectionConfigureMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "configure",		/* name */
    ConnectionConfigureMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};
const static Tcl_MethodType ConnectionRollbackMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "rollback",			/* name */
    ConnectionRollbackMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};
const static Tcl_MethodType ConnectionTablesMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "tables",			/* name */
    ConnectionTablesMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};
#endif
const static Tcl_MethodType* ConnectionMethods[] = {
//  &ConnectionBegintransactionMethodType,
//  &ConnectionColumnsMethodType,
//  &ConnectionCommitMethodType,
//  &ConnectionConfigureMethodType,
//  &ConnectionRollbackMethodType,
//  &ConnectionTablesMethodType,
    NULL
};

/* Method types of the statement methods that are implemented in C */

const static Tcl_MethodType StatementConstructorType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "CONSTRUCTOR",		/* name */
    StatementConstructor,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};
#if 0
const static Tcl_MethodType StatementParamsMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "params",			/* name */
    StatementParamsMethod,	/* callProc */
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
#endif
/* 
 * Methods to create on the statement class. 
 */

const static Tcl_MethodType* StatementMethods[] = {
//  &StatementParamsMethodType,
//  &StatementParamtypeMethodType,
    NULL
};

/* Method types of the result set methods that are implemented in C */

const static Tcl_MethodType ResultSetConstructorType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "CONSTRUCTOR",		/* name */
    ResultSetConstructor,	/* callProc */
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
#if 0
const static Tcl_MethodType ResultSetRowcountMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "rowcount",			/* name */
    ResultSetRowcountMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};
#endif

/* Methods to create on the result set class */

const static Tcl_MethodType* ResultSetMethods[] = {
//  &ResultSetColumnsMethodType,
//  &ResultSetRowcountMethodType,
    NULL
};


/* Initialization script */

static const char initScript[] =
    "namespace eval ::tdbc::oracle {}\n"
    "tcl_findLibrary tdbcoracle " PACKAGE_VERSION " " PACKAGE_VERSION
    " tdbcoracle.tcl TDBCORACLE_LIBRARY ::tdbc::oracle::Library";


/*
 *-----------------------------------------------------------------------------
 *
 * TransferOracleError --
 *
 *	Check if there is any error decribed by oci error handle. 
 *	If there was an error, it obtains error message, SQL state
 *	and error number from the OCI library and transfers
 *	them into the Tcl interpreter. 
 *
 * Results:
 *	TCL_OK if no error exists or the error was non fatal,
 *	otherwise TCL_ERROR is returned
 *
 * Side effects:
 *	Sets the interpreter result and error code to describe the SQL connection error.
 *
 *-----------------------------------------------------------------------------
 */

static int TransferOracleError(
	Tcl_Interp* interp,	/* Interpreter handle */
	OCIError* ociErrHp,	/* OCI error handle */
	sword status		/* Status retturned by last operation */
) {
    char* sqlState = "HY000";  
    char errMsg[1000]; 
    sb4 errorCode = status;
   
    if (status != OCI_SUCCESS) {  
	switch (status) {
	    case OCI_SUCCESS_WITH_INFO:
		if (OCIErrorGet(ociErrHp, 1, NULL, &errorCode,
			    (text*) errMsg, 1000, OCI_HTYPE_ERROR) != OCI_SUCCESS) {
		    strcpy(errMsg, "Cannot retreive OCI error message");
		}
		break;
	    case OCI_NEED_DATA:
		strcpy(errMsg, "OCI_NEED_DATA error occured\n");
		break;
	    case OCI_NO_DATA:
		strcpy(errMsg, "OCI_NO_DATA error occured\n");
		break;
	    case OCI_ERROR:
		if (OCIErrorGet(ociErrHp, 1, NULL, &errorCode,
			    (text*) errMsg, 1000, OCI_HTYPE_ERROR) != OCI_SUCCESS) {
		    strcpy(errMsg, "Cannot retreive OCI error message");
		}
		break;
	    case OCI_INVALID_HANDLE:
		strcpy(errMsg, "OCI_INVALID_HANDLE error occured\n");
		break;
	    case OCI_STILL_EXECUTING:
		strcpy(errMsg, "OCI_STILL_EXECUTING error occured\n");
		break;
	    case OCI_CONTINUE:
		strcpy(errMsg, "OCI_CONTINUE error occured\n");
		break;
	    default: 
		strcpy(errMsg, "Unknown error occured\n");
	}
    
	Tcl_Obj* errorCode = Tcl_NewObj();
	Tcl_ListObjAppendElement(NULL, errorCode, Tcl_NewStringObj("TDBC", -1));
	Tcl_ListObjAppendElement(NULL, errorCode,
		Tcl_NewStringObj(Tdbc_MapSqlState(sqlState), -1 )); 
	Tcl_ListObjAppendElement(NULL, errorCode,
		Tcl_NewStringObj(sqlState, -1));
	Tcl_ListObjAppendElement(NULL, errorCode, Tcl_NewStringObj("POSTGRES", -1));

	Tcl_ListObjAppendElement(NULL, errorCode,
		Tcl_NewIntObj(status));
	Tcl_SetObjErrorCode(interp, errorCode);
	Tcl_SetObjResult(interp, Tcl_NewStringObj(errMsg, -1));
    }

    if (status == OCI_SUCCESS || status == OCI_SUCCESS_WITH_INFO 
	    || status == OCI_CONTINUE) {
	return TCL_OK;
    } else {
	return TCL_ERROR; 
    }
}

/*
 *-----------------------------------------------------------------------------
 *
 * QueryConnectionOption --
 *
 *	Determine the current value of a connection option.
 *
 * Results:
 *	Returns a Tcl object containing the value if successful, or NULL
 *	if unsuccessful. If unsuccessful, stores error information in the
 *	Tcl interpreter.
 *
 *-----------------------------------------------------------------------------
 */

static Tcl_Obj*
QueryConnectionOption (
    ConnectionData* cdata,	/* Connection data */
    Tcl_Interp* interp,		/* Tcl interpreter */
    int optionNum		/* Position of the option in the table */
) {
    return NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * ConfigureConnection --
 *
 *	Applies configuration settings to a MySQL connection.
 *
 * Results:
 *	Returns a Tcl result. If the result is TCL_ERROR, error information
 *	is stored in the interpreter.
 *
 * Side effects:
 *	Updates configuration in the connection data. Opens a connection
 *	if none is yet open.
 *
 *-----------------------------------------------------------------------------
 */

static int
ConfigureConnection(
    PerInterpData* pidata,	/* Per interpreter data*/
    ConnectionData* cdata,	/* Connection data */
    Tcl_Interp* interp,		/* Tcl interpreter */
    int objc,			/* Parameter count */
    Tcl_Obj* const objv[],	/* Parameter data */
    int skip			/* Number of parameters to skip */
) {

    const char* stringOpts[INDX_MAX];
				/* String-valued options */
    int optionIndex;		/* Index of the current option in ConnOptions */
    sword status;		/* Result returned by OCI functions */
    int i;
    Tcl_Obj* retval;
    Tcl_Obj* optval;

    if (cdata->ociAutHp != NULL) {
	
	/* Query configuration options on an existing connection */

	if (objc == skip) {
	    retval = Tcl_NewObj();
	    for (i = 0; ConnOptions[i].name != NULL; ++i) {
		if (ConnOptions[i].flags & CONN_OPT_FLAG_ALIAS) continue;
		optval = QueryConnectionOption(cdata, interp, i);
		if (optval == NULL) {
		    return TCL_ERROR;
		}
		Tcl_DictObjPut(NULL, retval,
			       Tcl_NewStringObj(ConnOptions[i].name, -1),
			       optval);
	    }
	    Tcl_SetObjResult(interp, retval);
	    return TCL_OK;
	} else if (objc == skip+1) {

	    if (Tcl_GetIndexFromObjStruct(interp, objv[skip],
					  (void*) ConnOptions,
					  sizeof(ConnOptions[0]), "option",
					  0, &optionIndex) != TCL_OK) {
		return TCL_ERROR;
	    }
	    retval = QueryConnectionOption(cdata, interp, optionIndex);
	    if (retval == NULL) {
		return TCL_ERROR;
	    } else {
		Tcl_SetObjResult(interp, retval);
		return TCL_OK;
	    }
	}
    }

    if ((objc-skip) % 2 != 0) {
	Tcl_WrongNumArgs(interp, skip, objv, "?-option value?...");
	return TCL_ERROR;
    }

    /* Extract options from the command line */

    for (i = 0; i < INDX_MAX; ++i) {
	stringOpts[i] = NULL;
    }
    for (i = skip; i < objc; i += 2) {

	/* Unknown option */

	if (Tcl_GetIndexFromObjStruct(interp, objv[i], (void*) ConnOptions,
				      sizeof(ConnOptions[0]), "option",
				      0, &optionIndex) != TCL_OK) {
	    return TCL_ERROR;
	}

	/* Unmodifiable option */

	if (cdata->ociAutHp != NULL && !(ConnOptions[optionIndex].flags
					 & CONN_OPT_FLAG_MOD)) {
	    Tcl_Obj* msg = Tcl_NewStringObj("\"", -1);
	    Tcl_AppendObjToObj(msg, objv[i]);
	    Tcl_AppendToObj(msg, "\" option cannot be changed dynamically", -1);
	    Tcl_SetObjResult(interp, msg);
	    Tcl_SetErrorCode(interp, "TDBC", "GENERAL_ERROR", "HY000", 
			     "MYSQL", "-1", NULL);
	    return TCL_ERROR;
	}

	/* Record option value */

	switch (ConnOptions[optionIndex].type) {
	case TYPE_STRING:
	    stringOpts[ConnOptions[optionIndex].info] =
		Tcl_GetString(objv[i+1]);
	    break;
	}
    }
	
    if (cdata->ociAutHp == NULL) {

	/* Configuring a new connection.*/

	if (stringOpts[INDX_DBLINK] != NULL ) {
	    status = OCIServerAttach(cdata->ociSrvHp, cdata->ociErrHp, 
		    (text *) stringOpts[INDX_DBLINK],
		    strlen(stringOpts[INDX_DBLINK]), 0);
	} else {
	    status = OCIServerAttach(cdata->ociSrvHp, cdata->ociErrHp, 
		    (text *) "", strlen(""), 0);
	}
	if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK ) { 
	    return TCL_ERROR;
	}

	OCIAttrSet(cdata->ociSvcHp, OCI_HTYPE_SVCCTX,
	       	cdata->ociSrvHp, 0, OCI_ATTR_SERVER, cdata->ociErrHp);
	OCIHandleAlloc(pidata->ociEnvHp, (dvoid**) &cdata->ociAutHp,
		OCI_HTYPE_SESSION, 0, NULL);
	
	/* Set login data */

	if (stringOpts[INDX_USER] != NULL ) {
	    OCIAttrSet((dvoid *) cdata->ociAutHp, OCI_HTYPE_SESSION,
		    (void *) stringOpts[INDX_USER], strlen(stringOpts[INDX_USER]),
		    OCI_ATTR_USERNAME, cdata->ociErrHp);
	}

	if (stringOpts[INDX_PASS] != NULL ) {
	    OCIAttrSet(cdata->ociAutHp, OCI_HTYPE_SESSION,
		    (void *) stringOpts[INDX_PASS], strlen(stringOpts[INDX_PASS]),
		    OCI_ATTR_PASSWORD, cdata->ociErrHp);
	}

	/* Open the database */ 

	status = OCISessionBegin(cdata->ociSvcHp,  cdata->ociErrHp, 
		cdata->ociAutHp, OCI_CRED_RDBMS, OCI_DEFAULT);
    	if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK ) { 
	    return TCL_ERROR;
	}

	OCIAttrSet((dvoid *) cdata->ociSvcHp, OCI_HTYPE_SVCCTX,
		(dvoid *) cdata->ociAutHp, 0, OCI_ATTR_SESSION, cdata->ociErrHp);

    } else {

	/* Already open connection */

    }

    /* Setting of mutable presets */

    return TCL_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * ConnectionConstructor --
 *
 *	Constructor for ::tdbc::oracle::connection, which represents a
 *	database connection.
 *
 * Results:
 *	Returns a standard Tcl result.
 *
 * The ConnectionInitMethod takes alternating keywords and values giving
 * the configuration parameters of the connection, and attempts to connect
 * to the database.
 *
 *-----------------------------------------------------------------------------
 */

static int
ConnectionConstructor(
    ClientData clientData,	/* Environment handle */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context, /* Object context */
    int objc,			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    PerInterpData* pidata = (PerInterpData*) clientData;
				/* Per-interp data for the ORACLE package */
    Tcl_Object thisObject = Tcl_ObjectContextObject(context);
				/* The current object */
    int skip = Tcl_ObjectContextSkippedArgs(context);
				/* The number of leading arguments to skip */
    ConnectionData* cdata;	/* Per-connection data */

    /* Hang client data on this connection */

    cdata = (ConnectionData*) ckalloc(sizeof(ConnectionData));
    cdata->refCount = 1;
    cdata->pidata = pidata;
    cdata->flags = 0;
    cdata->ociAutHp = NULL; 
    IncrPerInterpRefCount(pidata);
    Tcl_ObjectSetMetadata(thisObject, &connectionDataType, (ClientData) cdata);
   

    /* Allocate OCI error, server and service handles */

    OCIHandleAlloc( (dvoid *) pidata->ociEnvHp, (dvoid **) &cdata->ociErrHp, OCI_HTYPE_ERROR,
	    (size_t) 0, NULL);
    OCIHandleAlloc( (dvoid *) pidata->ociEnvHp, (dvoid **) &cdata->ociSrvHp, OCI_HTYPE_SERVER,
	    (size_t) 0, NULL);
    OCIHandleAlloc( (dvoid *) pidata->ociEnvHp, (dvoid **) &cdata->ociSvcHp, OCI_HTYPE_SVCCTX,
	    (size_t) 0, NULL);


    /* Configure the connection */

    if (ConfigureConnection(pidata, cdata, interp, objc, objv, skip) != TCL_OK) {
	skip = TCL_ERROR;
	//TODO: o co tu chodzilo?
	return skip;
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
 *	down ORACLE when it is no longer required.
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
 *	Callback executed when any of the ORACLE client methods is cloned.
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
    *newClientData = oldClientData;
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
    if (cdata->ociAutHp) { 
	/* Disconnect from the database */
	//TODO: delete checks
	if (OCISessionEnd(cdata->ociSvcHp, cdata->ociErrHp,
		    cdata->ociAutHp, OCI_DEFAULT)!= OCI_SUCCESS) { 
	    printf("OCISessionEnd: not successful\n");
	}
	if (OCIServerDetach(cdata->ociSrvHp, cdata->ociErrHp,
		   OCI_DEFAULT) != OCI_SUCCESS) { 
	    printf("OCIServerDetach: not successful\n");
	}
	OCIHandleFree(cdata->ociAutHp, OCI_HTYPE_SESSION);

    }
    OCIHandleFree(cdata->ociErrHp, OCI_HTYPE_ERROR);
    OCIHandleFree(cdata->ociSvcHp, OCI_HTYPE_SVCCTX);
    OCIHandleFree(cdata->ociSrvHp, OCI_HTYPE_SERVER);
    
    DecrPerInterpRefCount(cdata->pidata);
    ckfree((char*) cdata);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CloneConnection --
 *
 *	Attempts to clone an ORACLE connection's metadata.
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
		     Tcl_NewStringObj("ORACLE connections are not clonable", -1));
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
    sdata->params = NULL;
    sdata->nativeSql = NULL;
    sdata->ociStmtHp = NULL;
    sdata->columnNames = NULL;
    sdata->flags = 0;
    return sdata;
}




/*
 *-----------------------------------------------------------------------------
 *
 * AllocAndPrepareStatement --
 *
 *	Allocate space for a MySQL prepared statement, and prepare the
 *	statement.
 *
 * Results:
 *	Returns the statement handle if successful, and NULL on failure.
 *
 * Side effects:
 *	Prepares the statement.
 *	Stores error message and error code in the interpreter on failure.
 *
 *-----------------------------------------------------------------------------
 */

static OCIStmt*
AllocAndPrepareStatement(
    Tcl_Interp* interp,		/* Tcl interpreter for error reporting */
    StatementData* sdata	/* Statement data */
) {
    ConnectionData* cdata = sdata->cdata;
				/* Connection data */
    OCIStmt* ociStmtHp;		/* Statement handle */
    const char* nativeSqlStr;	/* Native SQL statement to prepare */
    int nativeSqlLen;		/* Length of the statement */
    sword status;		/* Status returned by OCI functions */

    /* Allocate space for the prepared statement */

    status = OCIHandleAlloc(cdata->pidata->ociEnvHp,
	    (dvoid **) &ociStmtHp, OCI_HTYPE_STMT, 0, NULL);
    if (TransferOracleError(interp, cdata->ociErrHp, status)) {
	ociStmtHp = NULL;	
    } else {

	/* Prepare the statement */

	nativeSqlStr = Tcl_GetStringFromObj(sdata->nativeSql, &nativeSqlLen);
	status = OCIStmtPrepare(ociStmtHp, cdata->ociErrHp, (text*) nativeSqlStr, 
		strlen(nativeSqlStr), OCI_NTV_SYNTAX, OCI_DEFAULT);

	if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	    OCIHandleFree((dvoid *) ociStmtHp, OCI_HTYPE_STMT);
	    ociStmtHp =  NULL;
	}

    }
    return ociStmtHp;
}

/*
 *-----------------------------------------------------------------------------
 *
 * ResultDescToTcl --
 *
 *	Converts a MySQL result description for return as a Tcl list.
 *
 * Results:
 *	Returns a Tcl object holding the result description
 *
 * If any column names are duplicated, they are disambiguated by
 * appending '#n' where n increments once for each occurrence of the
 * column name.
 *
 *-----------------------------------------------------------------------------
 */

static Tcl_Obj*
ResultDescToTcl(
    ResultSetData* rdata,	/* Result set description */
    int flags			/* Flags governing the conversion */
) {
    Tcl_Obj* retval = Tcl_NewObj();
    Tcl_HashTable names;	/* Hash table to resolve name collisions */
    sword status;		/* Statur returned by OCI calls */
    OCIParam* ociParamH;	/* OCI parameter handle */
    Tcl_InitHashTable(&names, TCL_STRING_KEYS);
    if (rdata != NULL) {
	unsigned int i = 0;
	char numbuf[16];

	status = OCIParamGet(rdata->ociStmtHp, OCI_HTYPE_STMT, 
		rdata->sdata->cdata->ociErrHp, (dvoid**) &ociParamH, 0); 

	while (status != OCI_ERROR) { 
	    Tcl_Obj* nameObj;
	    int new;
	    Tcl_HashEntry* entry;
	    char * colName; 
	    ub4 colNameLen;
	    int count = 1;

	    OCIAttrGet(ociParamH, OCI_DTYPE_PARAM, &colName, 
		    &colNameLen, OCI_ATTR_NAME, rdata->sdata->cdata->ociErrHp); 
	    nameObj = Tcl_NewStringObj(colName, colNameLen);
	    Tcl_IncrRefCount(nameObj);
	    entry = Tcl_CreateHashEntry(&names, colName, &new);

	    while (!new) {
		count = (int) Tcl_GetHashValue(entry);
		++count;
		Tcl_SetHashValue(entry, (ClientData) count);
		sprintf(numbuf, "#%d", count);
		Tcl_AppendToObj(nameObj, numbuf, -1);
		entry = Tcl_CreateHashEntry(&names, Tcl_GetString(nameObj),
					    &new);
	    }
	    Tcl_SetHashValue(entry, (ClientData) count);
	    Tcl_ListObjAppendElement(NULL, retval, nameObj);
	    Tcl_DecrRefCount(nameObj);

	    if (OCIDescriptorFree(ociParamH, OCI_DTYPE_PARAM) != OCI_SUCCESS) {
		printf("OCIDescriptorFree(OCI_DTYPE_PARAM) failed\n");
	    }
	    
	    i += 1;
	    status = OCIParamGet(rdata->ociStmtHp, OCI_HTYPE_STMT, 
		    rdata->sdata->cdata->ociErrHp, (dvoid**)&ociParamH, i); 

	}
    }
    Tcl_DeleteHashTable(&names);
    return retval;
}



/*
 *-----------------------------------------------------------------------------
 *
 * StatementConstructor --
 *	statement.
 *
 * Usage:
 *	statement new connection statementText
 *	statement create name connection statementText
 *
 * Parameters:
 *      connection -- the Oracle connection object
 *	statementText -- text of the statement to prepare.
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
StatementConstructor(
    ClientData clientData,	/* Not used */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context,	/* Object context  */
    int objc, 			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    Tcl_Object thisObject = Tcl_ObjectContextObject(context);
				/* The current statement object */
    int skip = Tcl_ObjectContextSkippedArgs(context);
				/* Number of args to skip before the
				 * payload arguments */
    Tcl_Object connectionObject;
				/* The database connection as a Tcl_Object */
    ConnectionData* cdata;	/* The connection object's data */
    StatementData* sdata;	/* The statement's object data */
    Tcl_Obj* tokens;		/* The tokens of the statement to be prepared */
    int tokenc;			/* Length of the 'tokens' list */
    Tcl_Obj** tokenv;		/* Exploded tokens from the list */
    Tcl_Obj* nativeSql;		/* SQL statement mapped to native form */
    char* tokenStr;		/* Token string */
    int tokenLen;		/* Length of a token */
    int nParams;		/* Number of parameters of the statement */
    char*  tmpstr;		/* Temporary array for strings */

    int i;

    /* Find the connection object, and get its data. */

    thisObject = Tcl_ObjectContextObject(context);
    if (objc != skip+2) {
	Tcl_WrongNumArgs(interp, skip, objv, "connection statementText");
	return TCL_ERROR;
    }

    connectionObject = Tcl_GetObjectFromObj(interp, objv[skip]);
    if (connectionObject == NULL) {
	return TCL_ERROR;
    }
    cdata = (ConnectionData*) Tcl_ObjectGetMetadata(connectionObject,
						    &connectionDataType);
    if (cdata == NULL) {
	Tcl_AppendResult(interp, Tcl_GetString(objv[skip]),
			 " does not refer to a MySQL connection", NULL);
	return TCL_ERROR;
    }

    /*
     * Allocate an object to hold data about this statement
     */

    sdata = NewStatement(cdata);

    /* Tokenize the statement */

    tokens = Tdbc_TokenizeSql(interp, Tcl_GetString(objv[skip+1]));
    if (tokens == NULL) {
	goto freeSData;
    }
    Tcl_IncrRefCount(tokens);

    /*
     * Rewrite the tokenized statement to MySQL syntax. Reject the
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
	    tmpstr = ckalloc(strlen(tokenStr));
	    tmpstr[0] = ':';
	    Tcl_AppendToObj(nativeSql, tmpstr, -1);
	    ckfree(tmpstr);
	    Tcl_ListObjAppendElement(NULL, sdata->subVars, 
				     Tcl_NewStringObj(tokenStr+1, tokenLen-1));
	    break;

	case ';':
	    Tcl_SetObjResult(interp,
			     Tcl_NewStringObj("tdbc::oracle"
					      " does not support semicolons "
					      "in statements", -1));
	    goto freeNativeSql;
	    break; 

	default:
	    Tcl_AppendToObj(nativeSql, tokenStr, tokenLen);
	    break;

	}
    }
    sdata->nativeSql = nativeSql;
    Tcl_DecrRefCount(tokens);

    /* Prepare the statement */
    
    sdata->ociStmtHp = AllocAndPrepareStatement(interp, sdata);
    if (sdata->ociStmtHp == NULL) {
	goto freeSData;
    }


    Tcl_ListObjLength(NULL, sdata->subVars, &nParams);
    sdata->params = (ParamData*) ckalloc(nParams * sizeof(ParamData));
    for (i = 0; i < nParams; ++i) {
	sdata->params[i].flags = PARAM_IN;
	//TODO: varchar type number here
	sdata->params[i].dataType = -1;
	sdata->params[i].precision = 0;
	sdata->params[i].scale = 0;
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
 * DeleteStatementMetadata, DeleteStatement --
 *
 *	Cleans up when a Oracle statement is no longer required.
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
    if (sdata->columnNames != NULL) {
	Tcl_DecrRefCount(sdata->columnNames);
    }
    if (sdata->nativeSql != NULL) {
	Tcl_DecrRefCount(sdata->nativeSql);
    }
    if (sdata->params != NULL) {
	ckfree((char*)sdata->params);
    }
    Tcl_DecrRefCount(sdata->subVars);
    DecrConnectionRefCount(sdata->cdata);
    ckfree((char*)sdata);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CloneStatement --
 *
 *	Attempts to clone a Oracle statement's metadata.
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
		     Tcl_NewStringObj("Oracle statements are not clonable", -1));
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * ResultSetConstructor --
 *
 *	Constructs a new result set.
 *
 * Usage:
 *	$resultSet new statement ?dictionary?
 *	$resultSet create name statement ?dictionary?
 *
 * Parameters:
 *	statement -- Statement handle to which this resultset belongs
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
ResultSetConstructor(
    ClientData clientData,	/* Not used */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context,	/* Object context  */
    int objc, 			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    Tcl_Object thisObject = Tcl_ObjectContextObject(context);
				/* The current result set object */
    int skip = Tcl_ObjectContextSkippedArgs(context);
				/* Number of args to skip */
    Tcl_Object statementObject;	/* The current statement object */
    PerInterpData* pidata;	/* The per-interpreter data for this package */
    ConnectionData* cdata;	/* The MySQL connection object's data */
    StatementData* sdata;	/* The statement object's data */
    ResultSetData* rdata;	/* THe result set object's data */

    ub2* resultLengths;		/* Length of output fields */
    char** resultBindings;	/* Array of output field values */

    int nParams;		/* The parameter count on the statement */
    int nBound;			/* Number of parameters bound so far */
    int nDefined;   		/* Number of columns defined so far */
    Tcl_Obj* paramNameObj;	/* Name of the current parameter */
    const char* paramName;	/* Name of the current parameter */
    Tcl_Obj* paramValObj;	/* Value of the current parameter */
    char* paramValStr;	/* String value of the current parameter */
    int nColumns;		/* Number of columns in the result set */
    sword status;		/* Status returned by OCI calls */


    /* Check parameter count */

    if (objc != skip+1 && objc != skip+2) {
	Tcl_WrongNumArgs(interp, skip, objv, "statement ?dictionary?");
	return TCL_ERROR;
    }

    /* Initialize the base classes */

    Tcl_ObjectContextInvokeNext(interp, context, skip, objv, skip);

    /* Find the statement object, and get the statement data */

    statementObject = Tcl_GetObjectFromObj(interp, objv[skip]);
    if (statementObject == NULL) {
	return TCL_ERROR;
    }
    sdata = (StatementData*) Tcl_ObjectGetMetadata(statementObject,
						   &statementDataType);
    if (sdata == NULL) {
	Tcl_AppendResult(interp, Tcl_GetString(objv[skip]),
			 " does not refer to a MySQL statement", NULL);
	return TCL_ERROR;
    }
    cdata = sdata->cdata;

    pidata = cdata->pidata;

    /* Allocate an object to hold data about this result set */

    rdata = (ResultSetData*) ckalloc(sizeof(ResultSetData));
    rdata->refCount = 1;
    rdata->sdata = sdata;
    rdata->ociStmtHp = NULL;
    rdata->resultBindings = NULL; 
    rdata->resultLengths = NULL;

    IncrStatementRefCount(sdata);
    Tcl_ObjectSetMetadata(thisObject, &resultSetDataType, (ClientData) rdata);


    /*
     * Find a statement handle that we can use to execute the SQL code.
     * If the main statement handle associated with the statement
     * is idle, we can use it.  Otherwise, we have to allocate and
     * prepare a fresh one.
     */

    if (sdata->flags & STMT_FLAG_BUSY) {
	rdata->ociStmtHp = AllocAndPrepareStatement(interp, sdata);
	if (rdata->ociStmtHp == NULL) {
	    return TCL_ERROR;
	}
    } else {
	rdata->ociStmtHp = sdata->ociStmtHp;
	sdata->flags |= STMT_FLAG_BUSY;
    }

    Tcl_ListObjLength(NULL, sdata->subVars, &nParams);

    /* Bind the substituted parameters */

    for (nBound = 0; nBound < nParams; ++nBound) {
	Tcl_ListObjIndex(NULL, sdata->subVars, nBound, &paramNameObj);
	paramName = Tcl_GetString(paramNameObj);
	if (objc == skip+2) {

	    /* Param from a dictionary */

	    if (Tcl_DictObjGet(interp, objv[skip+1],
			       paramNameObj, &paramValObj) != TCL_OK) {
		return TCL_ERROR;
	    }
	} else {

	    /* Param from a variable */

	    paramValObj = Tcl_GetVar2Ex(interp, paramName, NULL, 
					TCL_LEAVE_ERR_MSG);
	}

	/* 
	 * At this point, paramValObj contains the parameter to bind.
	 * Convert the parameters to the appropriate data types for
	 * MySQL's prepared statement interface, and bind them.
	 */
	paramValStr = Tcl_GetString(paramValObj); 

	/* Bind value */

	status = OCIBindByPos(rdata->ociStmtHp, NULL, cdata->ociErrHp,
	       nBound, paramValStr, strlen(paramValStr), 0, NULL, NULL,
	       	NULL, 0 , NULL, OCI_DEFAULT);
	if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	    return TCL_ERROR;
	}
    }

    /* Execute the statement */

    //TODO: error possible on NON-SELECTS.
    status = OCIStmtExecute(cdata->ociSvcHp, rdata->ociStmtHp,
	    cdata->ociErrHp, 0, 0, NULL, NULL, OCI_DEFAULT);
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	return TCL_ERROR;
    }

    /* Get column names and count */
    if (sdata->columnNames == NULL) {
	sdata->columnNames = ResultDescToTcl(rdata, 0);
        Tcl_IncrRefCount(sdata->columnNames);
    }
    Tcl_ListObjLength(NULL, sdata->columnNames, &nColumns);

    /* Define Columns */

    resultBindings = rdata->resultBindings = (char**)ckalloc(
	    nColumns * sizeof(char*));
    resultLengths = rdata->resultLengths = (ub2*) ckalloc(nColumns * sizeof(ub2));

    for (nDefined = 0; nDefined < nColumns; nDefined += 1) { 
	int colSize; 
	OCIParam* ociParamH;

	resultBindings[nDefined] = NULL; 

	OCIParamGet(rdata->ociStmtHp, OCI_HTYPE_STMT, 
		rdata->sdata->cdata->ociErrHp, (dvoid**)&ociParamH, 0); 
	status = OCIAttrGet(ociParamH, OCI_DTYPE_PARAM, &colSize, 
		0, OCI_ATTR_DATA_SIZE, cdata->ociErrHp);
	
	if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {

	    OCIDescriptorFree(ociParamH, OCI_DTYPE_PARAM); 
	    goto freeRData;
	}
	
	OCIDescriptorFree(ociParamH, OCI_DTYPE_PARAM); 

	resultBindings[nDefined] = ckalloc(colSize);
	
	status = OCIDefineByPos(rdata->ociStmtHp, NULL, cdata->ociErrHp, nDefined, resultBindings[nDefined], colSize, 0, NULL, resultLengths, NULL, OCI_DEFAULT);
    }

    return TCL_OK;

    /* On error, unwind all the resource allocations */

freeRData:
    DecrStatementRefCount(sdata);
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * DeleteResultSetMetadata, DeleteResultSet --
 *
 *	Cleans up when a Oracle result set is no longer required.
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
    int nColumns; 
    int i; 

    if (rdata->resultLengths != NULL) {
        ckfree((char*)rdata->resultLengths);
    }

    if (rdata->resultBindings != NULL) {
	if (rdata->sdata->columnNames) { 
	    Tcl_ListObjLength(NULL, rdata->sdata->columnNames, &nColumns);
	    for (i=0; i<nColumns; i += 1) { 
		if (rdata->resultBindings[i] == NULL) 
		    break;
		ckfree(rdata->resultBindings[i]);
	    }
	    
	}
	ckfree((char*)rdata->resultBindings);
    }

    DecrStatementRefCount(rdata->sdata);
    ckfree((char*)rdata);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CloneResultSet --
 *
 *	Attempts to clone a Oracle result set's metadata.
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
		     Tcl_NewStringObj("Oracle result sets are not clonable",
				      -1));
    return TCL_ERROR;
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
	Tcl_WrongNumArgs(interp, 2, objv, "?pattern?");
	return TCL_ERROR;
    }

    Tcl_SetObjResult(interp, sdata->columnNames);

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

    int nColumns = 0;		/* Number of columns in the result set */
    Tcl_Obj* colName;		/* Name of the current column */
    Tcl_Obj* resultRow;		/* Row of the result set under construction */
    
    Tcl_Obj* colObj;		/* Column obtained from the row */
    int status = TCL_ERROR;	/* Status return from this command */
    int i, j;

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 2, objv, "varName");
	return TCL_ERROR;
    }


    /* Get the column names in the result set. */

    Tcl_ListObjLength(NULL, sdata->columnNames, &nColumns);
    if (nColumns == 0) {
	Tcl_SetObjResult(interp, literals[LIT_0]);
	return TCL_OK;
    }

    resultRow = Tcl_NewObj();
    Tcl_IncrRefCount(resultRow);

    not_imp;
}

/*
 *-----------------------------------------------------------------------------
 *
 * DeletePerInterpData --
 *
 *	Delete per-interpreter data when the ORACLE package is finalized
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

    Tcl_HashSearch search;
    Tcl_HashEntry *entry;
    for (entry = Tcl_FirstHashEntry(&(pidata->typeNumHash), &search);
	 entry != NULL;
	 entry = Tcl_NextHashEntry(&search)) {
	Tcl_Obj* nameObj = (Tcl_Obj*) Tcl_GetHashValue(entry);
	Tcl_DecrRefCount(nameObj);
    }
    Tcl_DeleteHashTable(&(pidata->typeNumHash));

    for (i = 0; i < LIT__END; ++i) {
	Tcl_DecrRefCount(pidata->literals[i]);
    }
//TODO: delete this check
    if (OCIHandleFree(pidata->ociEnvHp, OCI_HTYPE_ENV) != OCI_SUCCESS) {
	printf("OCIHandleFree(OCI_HTYPE_ENV) is not successful\n");
    }
    ckfree((char *) pidata);

    /* Free shared memory and deinitialize OCI library */ 

    OCITerminate(OCI_DEFAULT);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Tdbcoracle_Init --
 *
 *	Initializes the TDBC-ORACLE bridge when this library is loaded.
 *
 * Side effects:
 *	Creates the ::tdbc::oracle namespace and the commands that reside in it.
 *	Initializes the ORACLE environment.
 *
 *-----------------------------------------------------------------------------
 */

extern DLLEXPORT int
Tdbcoracle_Init(
    Tcl_Interp* interp		/* Tcl interpreter */
) {
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

    if (Tcl_PkgProvide(interp, "tdbc::oracle", PACKAGE_VERSION) == TCL_ERROR) {
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
    for (i = 0; i < LIT__END; ++i) {
	pidata->literals[i] = Tcl_NewStringObj(LiteralValues[i], -1);
	Tcl_IncrRefCount(pidata->literals[i]);
    }
    Tcl_InitHashTable(&(pidata->typeNumHash), TCL_ONE_WORD_KEYS);
    for (i = 0; dataTypes[i].name != NULL; ++i) {
	int new;
	Tcl_HashEntry* entry =
	    Tcl_CreateHashEntry(&(pidata->typeNumHash), 
				(const char*) (int) (dataTypes[i].num),
				&new);
	Tcl_Obj* nameObj = Tcl_NewStringObj(dataTypes[i].name, -1);
	Tcl_IncrRefCount(nameObj);
	Tcl_SetHashValue(entry, (ClientData) nameObj);
    }

    /* 
     * Find the connection class, and attach an 'init' method to it.
     */

    nameObj = Tcl_NewStringObj("::tdbc::oracle::connection", -1);
    Tcl_IncrRefCount(nameObj);
    if ((curClassObject = Tcl_GetObjectFromObj(interp, nameObj)) == NULL) {
	Tcl_DecrRefCount(nameObj);
	return TCL_ERROR;
    }
    Tcl_DecrRefCount(nameObj);
    curClass = Tcl_GetObjectAsClass(curClassObject);

    /* Attach the constructor to the 'connection' class */

    Tcl_ClassSetConstructor(interp, curClass,
			    Tcl_NewMethod(interp, curClass, NULL, 1,
					  &ConnectionConstructorType,
					  (ClientData) pidata));

    /* Attach the methods to the 'connection' class */

    for (i = 0; ConnectionMethods[i] != NULL; ++i) {
	nameObj = Tcl_NewStringObj(ConnectionMethods[i]->name, -1);
	Tcl_IncrRefCount(nameObj);
	Tcl_NewMethod(interp, curClass, nameObj, 1, ConnectionMethods[i],
			   (ClientData) NULL);
	Tcl_DecrRefCount(nameObj);
    }

    /* Look up the 'statement' class */

    nameObj = Tcl_NewStringObj("::tdbc::oracle::statement", -1);
    Tcl_IncrRefCount(nameObj);
    if ((curClassObject = Tcl_GetObjectFromObj(interp, nameObj)) == NULL) {
	Tcl_DecrRefCount(nameObj);
	return TCL_ERROR;
    }
    Tcl_DecrRefCount(nameObj);
    curClass = Tcl_GetObjectAsClass(curClassObject);

    /* Attach the constructor to the 'statement' class */

    Tcl_ClassSetConstructor(interp, curClass,
			    Tcl_NewMethod(interp, curClass, NULL, 1,
					  &StatementConstructorType,
					  (ClientData) NULL));

    /* Attach the methods to the 'statement' class */

    for (i = 0; StatementMethods[i] != NULL; ++i) {
	nameObj = Tcl_NewStringObj(StatementMethods[i]->name, -1);
	Tcl_IncrRefCount(nameObj);
	Tcl_NewMethod(interp, curClass, nameObj, 1, StatementMethods[i],
			   (ClientData) NULL);
	Tcl_DecrRefCount(nameObj);
    }

    /* Look up the 'resultSet' class */

    nameObj = Tcl_NewStringObj("::tdbc::oracle::resultset", -1);
    Tcl_IncrRefCount(nameObj);
    if ((curClassObject = Tcl_GetObjectFromObj(interp, nameObj)) == NULL) {
	Tcl_DecrRefCount(nameObj);
	return TCL_ERROR;
    }
    Tcl_DecrRefCount(nameObj);
    curClass = Tcl_GetObjectAsClass(curClassObject);

    /* Attach the constructor to the 'resultSet' class */

    Tcl_ClassSetConstructor(interp, curClass,
			    Tcl_NewMethod(interp, curClass, NULL, 1,
					  &ResultSetConstructorType,
					  (ClientData) NULL));

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

    /*
     * Initialize the Oracle library if this is the first interp using it
     */
    OCIInitialize(OCI_DEFAULT, NULL, NULL, NULL, NULL);
    
    (void) OCIEnvInit(&pidata->ociEnvHp, OCI_DEFAULT, 0, NULL);

    return TCL_OK;
}
