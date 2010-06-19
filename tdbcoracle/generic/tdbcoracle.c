//TODO: use OCIEnvCreate
//

/*
 * tdbcoracle.c --
 *
 *	Bridge between TDBC (Tcl DataBase Connectivity) and ORACLE.
 *
 * Copyright (c) 2009 Slawomir Cygan
 *
 * Please refer to the file, 'license.terms' for the conditions on
 * redistribution of this file and for a DISCLAIMER OF ALL WARRANTIES.
 *
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
    { "integer",    SQLT_NUM},
    { "varchar",    SQLT_CHR},
    { "numeric",    SQLT_INT},
    { "decimal",    SQLT_INT},
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
    int isolation;		/* Current isolation level */
    int readOnly;		/* Read only connection indicator */
    const char * ociDbLink;	/* OCI Database Link */ 
    const char * ociPassword;	/* OCI Database Password */ 
    OCIError*	ociErrHp;	/* OCI Error handle */
    OCIServer*	ociSrvHp;	/* OCI Server handle */
    OCISvcCtx*	ociSvcHp;	/* OCI Service handle */
    OCISession* ociAutHp;	/* OCI Session handle */
} ConnectionData;

/*
 * Flags for the state of an ORACLE connection
 */

#define CONN_FLAG_AUTOCOMMIT	0x1	/* Autocommit is set */
#define CONN_FLAG_IN_XCN	0x2 	/* Transaction is in progress */
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
    OCIStmt* ociStmtHp;		/* OCI statement Handle */ 
    ub2* definedLengths;	/* Length of output fields */
    char** definedValues;	/* Array of output field values */
    ub2* definedIndicators;	/* Indicators of per-column errors */
    int badCursorState;		/* Indicator of EOF condition */ 
    ub4 rowCount; 
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
    TYPE_ISOLATION,		/* Transaction isolation level */
    TYPE_READONLY,		/* Read-only indicator */
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
    { "-user",	    TYPE_STRING,    INDX_USER,	    0,			    NULL},
    { "-passwd",    TYPE_STRING,    INDX_PASS,	    0,			    NULL},
    { "-isolation", TYPE_ISOLATION, 0,		    CONN_OPT_FLAG_MOD,	NULL},
    { "-readonly",  TYPE_READONLY,  0,		    CONN_OPT_FLAG_MOD,	NULL},
    { NULL,	    0,		    0,		    0,			    NULL}
};

/* Tables of isolation levels: Tcl, SQL, and MySQL 'tx_isolation' */

static const char* TclIsolationLevels[] = {
    "readcommitted",
    "serializable",
    NULL
};

static const char* SqlIsolationLevels[] = {
    "SET TRANSACTION ISOLATION LEVEL READ COMMITTED",
    "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE",
    NULL
};

enum IsolationLevel {
    ISOL_READ_COMMITTED,
    ISOL_SERIALIZABLE,
    ISOL_NONE = -1
};

/* Default isolation level (from doc) */

#define DEFAULT_ISOL_LEVEL ISOL_READ_COMMITTED

/* Declarations of static functions appearing in this file */

static int ExecSimpleQuery(Tcl_Interp* interp, ConnectionData* cdata, const char * query);

static int TransferOracleError(Tcl_Interp* interp, OCIError* ociErrHp,	sword status);

static Tcl_Obj* QueryConnectionOption(ConnectionData* cdata, Tcl_Interp* interp,
				      int optionNum);
static int ConfigureConnection(ConnectionData* cdata, Tcl_Interp* interp,
			       int objc, Tcl_Obj *const objv[], int skip);
static int ConnectionConstructor(ClientData clientData, Tcl_Interp* interp,
				 Tcl_ObjectContext context,
				 int objc, Tcl_Obj *const objv[]);
static int ConnectionBegintransactionMethod(ClientData clientData,
					    Tcl_Interp* interp,
					    Tcl_ObjectContext context,
					    int objc, Tcl_Obj *const objv[]);
static int ConnectionColumnsMethod(ClientData clientData, Tcl_Interp* interp,
				  Tcl_ObjectContext context,
				  int objc, Tcl_Obj *const objv[]);
static int ConnectionCommitMethod(ClientData clientData, Tcl_Interp* interp,
				  Tcl_ObjectContext context,
				  int objc, Tcl_Obj *const objv[]);
static int ConnectionConfigureMethod(ClientData clientData, Tcl_Interp* interp,
				     Tcl_ObjectContext context,
				     int objc, Tcl_Obj *const objv[]);
static int ConnectionRollbackMethod(ClientData clientData, Tcl_Interp* interp,
				    Tcl_ObjectContext context,
				    int objc, Tcl_Obj *const objv[]);
static int ConnectionTablesMethod(ClientData clientData, Tcl_Interp* interp,
				  Tcl_ObjectContext context,
				  int objc, Tcl_Obj *const objv[]);

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
static int StatementParamtypeMethod(ClientData clientData, Tcl_Interp* interp,
				    Tcl_ObjectContext context,
				    int objc, Tcl_Obj *const objv[]);
static int StatementParamsMethod(ClientData clientData, Tcl_Interp* interp,
				 Tcl_ObjectContext context,
				 int objc, Tcl_Obj *const objv[]);

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
static int ResultSetRowcountMethod(ClientData clientData, Tcl_Interp* interp,
				   Tcl_ObjectContext context,
				   int objc, Tcl_Obj *const objv[]);

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
    "columns",			/* name */
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
const static Tcl_MethodType* ConnectionMethods[] = {
    &ConnectionBegintransactionMethodType,
    &ConnectionColumnsMethodType,
    &ConnectionCommitMethodType,
    &ConnectionConfigureMethodType,
    &ConnectionRollbackMethodType,
    &ConnectionTablesMethodType,
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
/* 
 * Methods to create on the statement class. 
 */

const static Tcl_MethodType* StatementMethods[] = {
    &StatementParamsMethodType,
    &StatementParamtypeMethodType,
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
const static Tcl_MethodType ResultSetRowcountMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "rowcount",			/* name */
    ResultSetRowcountMethod,	/* callProc */
    NULL,			/* deleteProc */
    NULL			/* cloneProc */
};

/* Methods to create on the result set class */

const static Tcl_MethodType* ResultSetMethods[] = {
    &ResultSetColumnsMethodType,
    &ResultSetRowcountMethodType,
    NULL
};

/*
 *-----------------------------------------------------------------------------
 *
 * ExecSimpleQuery --
 *	Executes given query. 
 * Results:
 *	TCL_OK on success or the error was non fatal,
 *	otherwise TCL_ERROR .
 *
 * Side effects:
 *	Sets the interpreter result and error code appropiately to
 *	query execution process
 *
 *-----------------------------------------------------------------------------
 */

static int ExecSimpleQuery(
	Tcl_Interp* interp,	    /* Tcl interpreter */
       	ConnectionData* cdata,	    /* Connection data handle */
       	const char * query	    /* Query to execute */
) {
    OCIStmt * ociStmtHp;	/* OCI statement handle */
    sword status;		/* Status returned from OCI calls */

    OCIHandleAlloc(cdata->pidata->ociEnvHp,
	    (dvoid **) &ociStmtHp, OCI_HTYPE_STMT, 0, NULL);

    status = OCIStmtPrepare(ociStmtHp, cdata->ociErrHp, (text*) query, 
		strlen(query), OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	goto freeHandle;
    }

    status = OCIStmtExecute(cdata->ociSvcHp, ociStmtHp,
	    cdata->ociErrHp, 1, 0, NULL, NULL, OCI_DEFAULT);
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	goto freeHandle;
    }

    OCIHandleFree((dvoid *) ociStmtHp, OCI_HTYPE_STMT);
    return TCL_OK;

freeHandle:
    OCIHandleFree((dvoid *) ociStmtHp, OCI_HTYPE_STMT);
    return TCL_ERROR;
}



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
    char sqlState[20];
    char errMsg[1000]; 
    sb4 errorCode = status;

    strcpy(sqlState, "HY000");
   
    if (status != OCI_SUCCESS) {  
	switch (status) {
	    case OCI_SUCCESS_WITH_INFO:
		if (OCIErrorGet(ociErrHp, 1, (text*)sqlState, &errorCode,
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
		if (OCIErrorGet(ociErrHp, 1, (text*)sqlState, &errorCode,
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
	Tcl_ListObjAppendElement(NULL, errorCode, Tcl_NewStringObj("ORACLE", -1));

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
    PerInterpData* pidata = cdata->pidata;
				/* Per-interpreter data */
    Tcl_Obj** literals = pidata->literals;
				/* Literal pool */
    char * str;			/* String containing attribute */
    ub4 strLen;			/* String length */
    Tcl_Obj* retval = NULL;	/* Return value */
    
    if (ConnOptions[optionNum].type == TYPE_STRING) {
	switch(ConnOptions[optionNum].info) {
	    case INDX_USER:
	       OCIAttrGet((dvoid *) cdata->ociAutHp, OCI_HTYPE_SESSION,
		       &str, &strLen, OCI_ATTR_USERNAME,
		       cdata->ociErrHp);
	       retval = Tcl_NewStringObj(str, strLen);
		break;
	    case INDX_PASS:
		if (cdata->ociPassword != NULL) {
    		    retval = Tcl_NewStringObj(cdata->ociPassword, -1);
		} else {
		    retval = literals[LIT_EMPTY];
		}

		break;
	    case INDX_DBLINK:
		if (cdata->ociDbLink != NULL) {
    		    retval = Tcl_NewStringObj(cdata->ociDbLink, -1);
		} else {
		    retval = literals[LIT_EMPTY];
		}
		break;
	}
    }

    if (ConnOptions[optionNum].type == TYPE_ISOLATION) {
	return Tcl_NewStringObj(
		TclIsolationLevels[cdata->isolation], -1);
    }

    if (ConnOptions[optionNum].type == TYPE_READONLY) {
	if (cdata->readOnly == 0) {
	    return literals[LIT_0];
	} else {
	    return literals[LIT_1];
	}
    }

    return retval;
}


/*
 *-----------------------------------------------------------------------------
 *
 * ConfigureConnection --
 *
 *	Applies configuration settings to a Oracle connection.
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
    ConnectionData* cdata,	/* Connection data */
    Tcl_Interp* interp,		/* Tcl interpreter */
    int objc,			/* Parameter count */
    Tcl_Obj* const objv[],	/* Parameter data */
    int skip			/* Number of parameters to skip */
) {

    const char* stringOpts[INDX_MAX];
				/* String-valued options */
    int optionIndex;		/* Index of the current option in ConnOptions */
    int isolation = ISOL_NONE;	/* Isolation level */
    int readOnly = -1;		/* Read only indicator */
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
			     "ORACLE", "-1", NULL);
	    return TCL_ERROR;
	}

	/* Record option value */

	switch (ConnOptions[optionIndex].type) {
	case TYPE_STRING:
	    stringOpts[ConnOptions[optionIndex].info] =
		Tcl_GetString(objv[i+1]);
	    break;
	case TYPE_ISOLATION:
	    if (Tcl_GetIndexFromObj(interp, objv[i+1], TclIsolationLevels,
				    "isolation level", TCL_EXACT, &isolation)
		!= TCL_OK) {
		return TCL_ERROR;
	    }
	    break;
	case TYPE_READONLY:
	    if (Tcl_GetBooleanFromObj(interp, objv[i+1], &readOnly)
		!= TCL_OK) {
		return TCL_ERROR;
	    }
	    break;
	}
    }
	
    if (cdata->ociAutHp == NULL) {

	/* Configuring a new connection.*/

	if (stringOpts[INDX_DBLINK] != NULL ) {
	    cdata->ociDbLink = stringOpts[INDX_DBLINK];
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
	OCIHandleAlloc(cdata->pidata->ociEnvHp, (dvoid**) &cdata->ociAutHp,
		OCI_HTYPE_SESSION, 0, NULL);
	
	/* Set login and other parameters */	

	if (stringOpts[INDX_USER] != NULL ) {
	    OCIAttrSet((dvoid *) cdata->ociAutHp, OCI_HTYPE_SESSION,
		    (void *) stringOpts[INDX_USER], strlen(stringOpts[INDX_USER]),
		    OCI_ATTR_USERNAME, cdata->ociErrHp);
	}

	if (stringOpts[INDX_PASS] != NULL ) {
	    cdata->ociPassword = stringOpts[INDX_PASS];
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

	cdata->flags |= CONN_FLAG_AUTOCOMMIT;

    } else {

	/* Already open connection */

    }

    /* Transaction isolation level */

    if (isolation != ISOL_NONE) {

	/* Oracle requires "SET TRANSACTION" to be first query in transaction.
	 * So, if this driver is not in explicit transaction, we tell oracle
	 * to do an empty commit, just to start a new transaction, so no error
	 * will be raised. 
	 * Otherwise, if in explicit transaction, we let Oracle to decide, if
	 * this is the right moment to do "SET TRANSACTION" */
    
	if (!(cdata->flags & CONN_FLAG_IN_XCN)) {
	    status = OCITransCommit(cdata->ociSvcHp, cdata->ociErrHp, OCI_DEFAULT); 
	    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
		return TCL_ERROR;
	    }
	}

	if (ExecSimpleQuery(interp, cdata,
		    SqlIsolationLevels[isolation]) != TCL_OK) {
	    return TCL_ERROR;
	}
	cdata->isolation = isolation;
    }

    /* Readonly indicator */

    if (readOnly != -1) {
	
	/* Oracle requires "SET TRANSACTION" to be first query in transaction.
	 * So, if this driver is not in explicit transaction, we tell oracle
	 * to do an empty commit, just to start a new transaction, so no error
	 * will be raised. 
	 * Otherwise, if in explicit transaction, we let Oracle to decide, if
	 * this is the right moment to do "SET TRANSACTION" */

        if (!(cdata->flags & CONN_FLAG_IN_XCN)) {
	    status = OCITransCommit(cdata->ociSvcHp, cdata->ociErrHp, OCI_DEFAULT); 
	    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
		return TCL_ERROR;
	    }
	}

	if (readOnly == 0) {
	    if (ExecSimpleQuery(interp, cdata, 
			"SET TRANSACTION READ WRITE") != TCL_OK) {
		return TCL_ERROR;
	    }
	} else {
	    if (ExecSimpleQuery(interp, cdata, 
			"SET TRANSACTION READ ONLY") != TCL_OK) {
		return TCL_ERROR;
	    }

	}
	cdata->readOnly = readOnly; 
    }


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
    cdata->ociDbLink = NULL;
    cdata->ociPassword = NULL;
    cdata->readOnly = 0; 
    cdata->isolation = DEFAULT_ISOL_LEVEL;
    IncrPerInterpRefCount(pidata);
    Tcl_ObjectSetMetadata(thisObject, &connectionDataType, (ClientData) cdata);
   

    /* Allocate OCI error, server and service handles */

    OCIHandleAlloc( (dvoid *) pidata->ociEnvHp, (dvoid **) &cdata->ociErrHp, OCI_HTYPE_ERROR,
	    0, NULL);
    OCIHandleAlloc( (dvoid *) pidata->ociEnvHp, (dvoid **) &cdata->ociSrvHp, OCI_HTYPE_SERVER,
	    0, NULL);
    OCIHandleAlloc( (dvoid *) pidata->ociEnvHp, (dvoid **) &cdata->ociSvcHp, OCI_HTYPE_SVCCTX,
	    0, NULL);


    /* Configure the connection */

    if (ConfigureConnection(cdata, interp, objc, objv, skip) != TCL_OK) {
	skip = TCL_ERROR;
	//TODO: o co tu chodzilo?
	return skip;
    }

    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * ConnectionBegintransactionMethod --
 *
 *	Method that requests that following operations on an Oracle connection
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
ConnectionBegintransactionMethod(
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

    if (cdata->flags & CONN_FLAG_IN_XCN) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj("Oracle does not support "
						  "nested transactions", -1));
	Tcl_SetErrorCode(interp, "TDBC", "GENERAL_ERROR", "HYC00",
			 "ORACLE", "-1", NULL);
	return TCL_ERROR;
    }
    cdata->flags |= CONN_FLAG_IN_XCN;

    /* Turn off autocommit for the duration of the transaction */
    
    cdata->flags &= ~CONN_FLAG_AUTOCOMMIT;
    
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
 * ConnectionColumnsMethod --
 *
 *	Method that asks for the names of columns in a table
 *	in the database (optionally matching a given pattern)
 *
 * Usage:
 * 	$connection columns table ?pattern?
 * 
 * Parameters:
 *	None.
 *
 * Results:
 *	Returns the list of tables
 *
 *-----------------------------------------------------------------------------
 */

static int
ConnectionColumnsMethod(
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
    PerInterpData* pidata = cdata->pidata;
				/* Per-interpreter data */
    Tcl_Obj** literals = pidata->literals;
				/* Literal pool */
    char* patternStr;		/* Pattern to match table names */
    Tcl_Obj* retval;		/* List of table names */
    Tcl_Obj* name;		/* Name of a column */
    Tcl_Obj* attrs;		/* Attributes of the column */
    Tcl_HashEntry* entry;	/* Hash entry for data type */
    OCIDescribe *ociDscHp = NULL;
				/* OCI describe handle */
    OCIParam *ociTParmH;        /* Table parameter handle */
    OCIParam *ociColLstH;	/* Column list parameter handle */
    OCIParam *ociColH;		/* Column handle */
    ub2 numCols;		/* Number of columns */
    sword status;		/* Status returned by OCI calls */
    int i;

    /* Check parameters */

    if (objc == 3) {
	patternStr = NULL;
    } else if (objc == 4) {
	int escape = 0; 
	patternStr = ckalloc(strlen(Tcl_GetString(objv[3])));
	strcpy(patternStr, Tcl_GetString(objv[3]));

	/* We must change SLQ wildcards to TCL ones, 
	 * as pattern matching will be done by Tcl_StringCaseMatch*/

	for (i=0; i<strlen(patternStr); i++) {
	    if (escape == 0) {
		if (patternStr[i] == '\\') {
		    escape = 1; 
		} else {
		    switch (patternStr[i]) {
			case '%':
			    patternStr[i] = '*';
			    break;
			case '_':
			    patternStr[i] = '?';
			    break;
		    }
		}
	    } else {
		escape = 0; 
	    }
	}
    } else {
	Tcl_WrongNumArgs(interp, 2, objv, "table ?pattern?");
	return TCL_ERROR;
    }

    OCIHandleAlloc(pidata->ociEnvHp, (dvoid **)&ociDscHp,
	        OCI_HTYPE_DESCRIBE, 0, NULL);

    status = OCIDescribeAny(cdata->ociSvcHp, cdata->ociErrHp, 
	    Tcl_GetString(objv[2]), strlen(Tcl_GetString(objv[2])), 
	    OCI_OTYPE_NAME, OCI_DEFAULT, OCI_PTYPE_TABLE, ociDscHp);
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	goto freeDesc;
    }
    status = OCIAttrGet(ociDscHp, OCI_HTYPE_DESCRIBE, &ociTParmH, 0, 
	    OCI_ATTR_PARAM, cdata->ociErrHp);
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	goto freeDesc;;
    }
    status = OCIAttrGet(ociTParmH, OCI_DTYPE_PARAM, &numCols, 0, 
	    OCI_ATTR_NUM_COLS, cdata->ociErrHp); 
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	goto freeTParmH;
    }
    OCIAttrGet(ociTParmH, OCI_DTYPE_PARAM, &ociColLstH, 0, 
	    OCI_ATTR_LIST_COLUMNS, cdata->ociErrHp);
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	goto freeColLstH;
    }
    retval = Tcl_NewObj();
    Tcl_IncrRefCount(retval);
    for (i = 0; i < numCols; ++i) {
	char* nameStr, * nameStrLower; 
	ub4 nameStrLen; 
	ub2 dataType;
	ub1 precision;
	ub2 charSize;
	ub1 nullable;
	sb1 scale;
	attrs = Tcl_NewObj();
	status = OCIParamGet(ociColLstH, OCI_DTYPE_PARAM,
		cdata->ociErrHp, (dvoid**) &ociColH, i + 1);
	if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	    goto freeColLstH;
	}

	status = OCIAttrGet(ociColH, OCI_DTYPE_PARAM, &nameStr, &nameStrLen,
		OCI_ATTR_NAME, cdata->ociErrHp); 
	if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	    OCIDescriptorFree(ociColH, OCI_DTYPE_PARAM);
	    goto freeColLstH;
	}
	nameStrLower = ckalloc(nameStrLen);
	strcpy(nameStrLower, nameStr);
	nameStrLower[nameStrLen] = '\0';
	Tcl_UtfToLower(nameStrLower);
	name = Tcl_NewStringObj(nameStrLower, -1);
	ckfree(nameStrLower);
	if (patternStr != NULL) {
	    if (Tcl_StringCaseMatch(Tcl_GetString(name), patternStr, TCL_MATCH_NOCASE) == 0) {
		OCIDescriptorFree(ociColH, OCI_DTYPE_PARAM);
		continue;
	    }
	}

	Tcl_DictObjPut(NULL, attrs, literals[LIT_NAME], name);

	OCIAttrGet(ociColH, OCI_DTYPE_PARAM, &dataType, 0, 
		OCI_ATTR_DATA_TYPE, cdata->ociErrHp);
	entry = Tcl_FindHashEntry(&(pidata->typeNumHash),
				  (char*) (int)dataType);
	if (entry != NULL) {
	    Tcl_DictObjPut(NULL, attrs, literals[LIT_TYPE],
			   (Tcl_Obj*) Tcl_GetHashValue(entry));
	} else { 
	    //toDO: delete it 
	    printf("new datatype %d\n", (int)dataType);
	}
	OCIAttrGet(ociColH, OCI_DTYPE_PARAM, &precision, 0, 
		OCI_ATTR_PRECISION, cdata->ociErrHp);
	if (precision != 0) {
	    Tcl_DictObjPut(NULL, attrs, literals[LIT_PRECISION],
		    Tcl_NewIntObj(precision));
	} else {
	    OCIAttrGet(ociColH, OCI_DTYPE_PARAM, &charSize, 0,
		    OCI_ATTR_CHAR_SIZE, cdata->ociErrHp);
	    Tcl_DictObjPut(NULL, attrs, literals[LIT_PRECISION],
		    Tcl_NewIntObj(charSize));
	}

	OCIAttrGet(ociColH, OCI_DTYPE_PARAM, &scale, 0, 
		OCI_ATTR_SCALE, cdata->ociErrHp);
	if (scale != -127) {
	    Tcl_DictObjPut(NULL, attrs, literals[LIT_SCALE],
		     Tcl_NewIntObj(scale));
	}
    
	OCIAttrGet(ociColH, OCI_DTYPE_PARAM, &nullable, 0, 
		OCI_ATTR_IS_NULL, cdata->ociErrHp);
	Tcl_DictObjPut(NULL, attrs, literals[LIT_NULLABLE],
		       Tcl_NewIntObj(nullable != 0));
	Tcl_DictObjPut(NULL, retval, name, attrs);

	OCIDescriptorFree(ociColH, OCI_DTYPE_PARAM);
    }
    Tcl_SetObjResult(interp, retval);
    Tcl_DecrRefCount(retval);

    OCIDescriptorFree(ociColLstH, OCI_DTYPE_PARAM);
    OCIDescriptorFree(ociTParmH, OCI_DTYPE_PARAM);
    if (patternStr != NULL) {
	ckfree(patternStr);
    }
    return TCL_OK;
  
freeColLstH:
    OCIDescriptorFree(ociColLstH, OCI_DTYPE_PARAM);
freeTParmH:
    OCIDescriptorFree(ociTParmH, OCI_DTYPE_PARAM);
freeDesc:
    OCIHandleFree(ociDscHp, OCI_HTYPE_DESCRIBE);
    if (patternStr != NULL) {
	ckfree(patternStr);
    }
    
    return TCL_ERROR;
}


/*
 *-----------------------------------------------------------------------------
 *
 * ConnectionCommitMethod --
 *
 *	Method that requests that a pending transaction against a database
 * 	be committed.
 *
 * Usage:
 *	$connection commit
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
ConnectionCommitMethod(
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
    sword status;			/* Oracle status return */

    /* Check parameters */

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 2, objv, "");
	return TCL_ERROR;
    }

    /* Reject the request if no transaction is in progress */

    if (!(cdata->flags & CONN_FLAG_IN_XCN)) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj("no transaction is in "
						  "progress", -1));
	Tcl_SetErrorCode(interp, "TDBC", "GENERAL_ERROR", "HY010",
			 "ORACLE", "-1", NULL);
	return TCL_ERROR;
    }

    /* End transaction, turn off "transaction in progress", and report status */
    status = OCITransCommit(cdata->ociSvcHp, cdata->ociErrHp, OCI_DEFAULT); 
    cdata->flags &= ~ CONN_FLAG_IN_XCN;
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	return TCL_ERROR;
    }
    return TCL_OK;
}

/*-----------------------------------------------------------------------------
 *
 * ConnectionConfigureMethod --
 *
 *	Change configuration parameters on an open connection.
 *
 * Usage:
 *	$connection configure ?-keyword? ?value? ?-keyword value ...?
 *
 * Parameters:
 *	Keyword-value pairs (or a single keyword, or an empty set)
 *	of configuration options.
 *
 * Options:
 *	The following options are supported;
 *	    -database
 *		Name of the database to use by default in queries
 *	    -encoding
 *		Character encoding to use with the server. (Must be utf-8)
 *	    -isolation
 *		Transaction isolation level.
 *	    -readonly
 *		Read-only flag (must be a false Boolean value)
 *	    -timeout
 *		Timeout value (both wait_timeout and interactive_timeout)
 *
 *	Other options supported by the constructor are here in read-only
 *	mode; any attempt to change them will result in an error.
 *
 *-----------------------------------------------------------------------------
 */

static int ConnectionConfigureMethod(
     ClientData clientData, 
     Tcl_Interp* interp,
     Tcl_ObjectContext objectContext,
     int objc, 
     Tcl_Obj *const objv[]
) {
    Tcl_Object thisObject = Tcl_ObjectContextObject(objectContext);
				/* The current connection object */
    int skip = Tcl_ObjectContextSkippedArgs(objectContext);
				/* Number of arguments to skip */
    ConnectionData* cdata = (ConnectionData*)
	Tcl_ObjectGetMetadata(thisObject, &connectionDataType);
				/* Instance data */
    return ConfigureConnection(cdata, interp, objc, objv, skip);
}



/*
 *-----------------------------------------------------------------------------
 *
 * ConnectionRollbackMethod --
 *
 *	Method that requests that a pending transaction against a database
 * 	be rolled back.
 *
 * Usage:
 * 	$connection rollback
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
ConnectionRollbackMethod(
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
    sword status;		/* Result code from Oracle operations */

    /* Check parameters */

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 2, objv, "");
	return TCL_ERROR;
    }

    /* Reject the request if no transaction is in progress */

    if (!(cdata->flags & CONN_FLAG_IN_XCN)) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj("no transaction is in "
						  "progress", -1));
	Tcl_SetErrorCode(interp, "TDBC", "GENERAL_ERROR", "HY010",
			 "ORACLE", "-1", NULL);
	return TCL_ERROR;
    }

    /* End transaction, turn off "transaction in progress", and report status */
    status = OCITransRollback(cdata->ociSvcHp, cdata->ociErrHp, OCI_DEFAULT); 
    cdata->flags &= ~CONN_FLAG_IN_XCN;
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	return TCL_ERROR;
    }
    return TCL_OK;
}
/*
 *-----------------------------------------------------------------------------
 *
 * ConnectionTablesMethod --
 *
 *	Method that asks for the names of tables in the database (optionally
 *	matching a given pattern
 *
 * Usage:
 * 	$connection tables ?pattern?
 * 
 * Parameters:
 *	None.
 *
 * Results:
 *	Returns the list of tables
 *
 *-----------------------------------------------------------------------------
 */

static int
ConnectionTablesMethod(
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
    Tcl_Obj** literals = cdata->pidata->literals;
				/* Literal pool */
    char* patternStr;		/* Pattern to match table names */
    Tcl_Obj* retval;		/* List of table names */
    OCIStmt * ociStmtHp;	/* OCI statement handle */
    OCIParam* ociParamH;	/* OCI parameter handle */
    OCIDefine* ociDef = NULL;	/* OCI output placeholder define handle */
    OCIBind* ociBind = NULL;	/* OCI parameter define handle */

    const char * sqlQuery = "SELECT table_name FROM user_tables \
			     WHERE table_name LIKE :pattern"; 
				/* SQL query for retrieving table names */
    char * tableName;		/* Actual table name */
    ub2 tableNameLen;		/* Length of actual table name */
    ub2 colSize;		/* Actual column size */
    sword status;		/* Status returned from OCI calls */

    /* Check parameters */

    if (objc == 2) {
	patternStr = ckalloc(strlen("%"));
	strcpy(patternStr, "%");
    } else if (objc == 3) {
	const char * patternTmp; 
	patternTmp = Tcl_GetString(objv[2]);
	patternStr = ckalloc(strlen(patternTmp));
	strcpy(patternStr, patternTmp);
	Tcl_UtfToUpper(patternStr);	
    } else {
	Tcl_WrongNumArgs(interp, 2, objv, "");
	return TCL_ERROR;
    }

    OCIHandleAlloc(cdata->pidata->ociEnvHp,
	    (dvoid **) &ociStmtHp, OCI_HTYPE_STMT, 0, NULL);

    status = OCIStmtPrepare(ociStmtHp, cdata->ociErrHp, (text*) sqlQuery, 
		strlen(sqlQuery), OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	goto freeStmt;
    }

    status = OCIBindByPos(ociStmtHp, &ociBind, cdata->ociErrHp,
	    1, patternStr, strlen(patternStr) + 1, SQLT_STR, NULL, NULL,
	    NULL, 0 , NULL, OCI_DEFAULT);
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	goto freeStmt;
    }
  
    status = OCIStmtExecute(cdata->ociSvcHp, ociStmtHp,
	    cdata->ociErrHp, 0, 0, NULL, NULL, OCI_DEFAULT);
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	goto freeStmt;
    }

    OCIParamGet(ociStmtHp, OCI_HTYPE_STMT, 
	    cdata->ociErrHp, (dvoid**)&ociParamH, 1); 
    status = OCIAttrGet(ociParamH, OCI_DTYPE_PARAM, &colSize, 
	    0, OCI_ATTR_DATA_SIZE, cdata->ociErrHp);
    
    OCIDescriptorFree(ociParamH, OCI_DTYPE_PARAM); 
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	goto freeStmt;
    }

    tableName = ckalloc(colSize);

    status = OCIDefineByPos(ociStmtHp, &ociDef,
	    cdata->ociErrHp, 1, tableName, colSize, SQLT_STR, NULL,
	    &tableNameLen, NULL, OCI_DEFAULT);
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	goto freeStmt;
    }
    retval = Tcl_NewObj();
    Tcl_IncrRefCount(retval);

    while (1) {
	char * tableNameLower;
	status = OCIStmtFetch(ociStmtHp, cdata->ociErrHp, 1, OCI_FETCH_NEXT, OCI_DEFAULT);
	if (status == OCI_NO_DATA) { 
	    break;
	} else {
	    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
		goto freeRet;
	    }
	}
	tableNameLower = ckalloc(tableNameLen);
	strncpy(tableNameLower, tableName, tableNameLen);
	tableNameLower[tableNameLen] = '\0';
	Tcl_UtfToLower(tableNameLower);
	Tcl_ListObjAppendElement(NULL, retval,
		Tcl_NewStringObj(tableNameLower, -1));
	ckfree(tableNameLower);
	Tcl_ListObjAppendElement(NULL, retval, literals[LIT_EMPTY]);

    }
    OCIHandleFree((dvoid *) ociStmtHp, OCI_HTYPE_STMT);
    Tcl_SetObjResult(interp, retval);
    Tcl_DecrRefCount(retval);
    ckfree(tableName);
    ckfree(patternStr);
    return TCL_OK;

freeRet:
    ckfree(tableName);
    Tcl_DecrRefCount(retval);
freeStmt:
    OCIHandleFree((dvoid *) ociStmtHp, OCI_HTYPE_STMT);
    ckfree(patternStr);
    return TCL_ERROR;
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
 *	Allocate space for a Oracle prepared statement, and prepare the
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
 *	Converts a Oracle result description for return as a Tcl list.
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
	unsigned int i = 1;
	char numbuf[16];
	
	status = OCIParamGet(rdata->ociStmtHp, OCI_HTYPE_STMT, 
   		rdata->sdata->cdata->ociErrHp, (dvoid**) &ociParamH, i); 

	/* Iterate through columns until end is reached */
	
	while (status != OCI_ERROR) { 
	    Tcl_Obj* nameObj;
	    int new;
	    Tcl_HashEntry* entry;
	    char* colNameTmp = NULL;
	    char* colName;
	    ub4 colNameLen;
	    int count = 1;

	    OCIAttrGet(ociParamH, OCI_DTYPE_PARAM, &colNameTmp, 
		    &colNameLen, OCI_ATTR_NAME, rdata->sdata->cdata->ociErrHp); 

	    /* Column names reported by OCI need to be convererted to lower case */

	    colName = ckalloc(colNameLen);
	    strncpy(colName, colNameTmp, colNameLen);
	    colName[colNameLen] = '\0'; 
	    Tcl_UtfToLower(colName);

	    nameObj = Tcl_NewStringObj(colName, colNameLen);
	    Tcl_IncrRefCount(nameObj);
	    entry = Tcl_CreateHashEntry(&names, colName, &new);

	    ckfree(colName);

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
			 " does not refer to a Oracle connection", NULL);
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
     * Rewrite the tokenized statement to Oracle syntax. Reject the
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
	    strcpy(tmpstr, tokenStr);
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
 * StatementParamtypeMethod --
 *
 *	Defines a parameter type in a Oracle statement.
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
    StatementData* sdata	/* The current statement */
	= (StatementData*) Tcl_ObjectGetMetadata(thisObject,
						 &statementDataType);
    struct {
	const char* name;
	int flags;
    } directions[] = {
	{ "in", 	PARAM_IN },
	{ "out",	PARAM_OUT },
	{ "inout",	PARAM_IN | PARAM_OUT },
	{ NULL,		0 }
    };
    int direction;
    int typeNum;		/* Data type number of a parameter */
    int precision;		/* Data precision */
    int scale;			/* Data scale */

    int nParams;		/* Number of parameters to the statement */
    const char* paramName;	/* Name of the parameter being set */
    Tcl_Obj* targetNameObj;	/* Name of the ith parameter in the statement */
    const char* targetName;	/* Name of a candidate parameter in the
				 * statement */
    int matchCount = 0;		/* Number of parameters matching the name */
    Tcl_Obj* errorObj;		/* Error message */

    int i;

    /* Check parameters */

    if (objc < 4) {
	goto wrongNumArgs;
    }
    
    i = 3;
    if (Tcl_GetIndexFromObjStruct(interp, objv[i], directions, 
				  sizeof(directions[0]), "direction",
				  TCL_EXACT, &direction) != TCL_OK) {
	direction = PARAM_IN;
	Tcl_ResetResult(interp);
    } else {
	++i;
    }
    if (i >= objc) goto wrongNumArgs;
    if (Tcl_GetIndexFromObjStruct(interp, objv[i], dataTypes,
				  sizeof(dataTypes[0]), "SQL data type",
				  TCL_EXACT, &typeNum) == TCL_OK) {
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

    /* Look up parameters by name. */

    Tcl_ListObjLength(NULL, sdata->subVars, &nParams);
    paramName = Tcl_GetString(objv[2]);
    for (i = 0; i < nParams; ++i) {
	Tcl_ListObjIndex(NULL, sdata->subVars, i, &targetNameObj);
	targetName = Tcl_GetString(targetNameObj);
	if (!strcmp(paramName, targetName)) {
	    ++matchCount;
	    sdata->params[i].flags = direction;
	    sdata->params[i].dataType = dataTypes[typeNum].num;
	    sdata->params[i].precision = precision;
	    sdata->params[i].scale = scale;
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
 * StatementParamsMethod --
 *
 *	Lists the parameters in a Oracle statement.
 *
 * Usage:
 *	$statement params
 *
 * Results:
 *	Returns a standard Tcl result containing a dictionary. The keys
 *	of the dictionary are parameter names, and the values are parameter
 *	types, themselves expressed as dictionaries containing the keys,
 *	'name', 'direction', 'type', 'precision', 'scale' and 'nullable'.
 *
 *
 *-----------------------------------------------------------------------------
 */

static int
StatementParamsMethod(
    ClientData clientData,	/* Not used */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext context,	/* Object context  */
    int objc, 			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    Tcl_Object thisObject = Tcl_ObjectContextObject(context);
				/* The current statement object */
    StatementData* sdata	/* The current statement */
	= (StatementData*) Tcl_ObjectGetMetadata(thisObject,
						 &statementDataType);
    ConnectionData* cdata = sdata->cdata;
    PerInterpData* pidata = cdata->pidata; /* Per-interp data */
    Tcl_Obj** literals = pidata->literals; /* Literal pool */
    int nParams;		/* Number of parameters to the statement */
    Tcl_Obj* paramName;		/* Name of a parameter */
    Tcl_Obj* paramDesc;		/* Description of one parameter */
    Tcl_Obj* dataTypeName;	/* Name of a parameter's data type */
    Tcl_Obj* retVal;		/* Return value from this command */
    Tcl_HashEntry* typeHashEntry;
    int i;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 2, objv, "");
	return TCL_ERROR;
    }

    retVal = Tcl_NewObj();
    Tcl_ListObjLength(NULL, sdata->subVars, &nParams);
    for (i = 0; i < nParams; ++i) {
	paramDesc = Tcl_NewObj();
	Tcl_ListObjIndex(NULL, sdata->subVars, i, &paramName);
	Tcl_DictObjPut(NULL, paramDesc, literals[LIT_NAME], paramName);
	switch (sdata->params[i].flags & (PARAM_IN | PARAM_OUT)) {
	case PARAM_IN:
	    Tcl_DictObjPut(NULL, paramDesc, literals[LIT_DIRECTION], 
			   literals[LIT_IN]);
	    break;
	case PARAM_OUT:
	    Tcl_DictObjPut(NULL, paramDesc, literals[LIT_DIRECTION], 
			   literals[LIT_OUT]);
	    break;
	case PARAM_IN | PARAM_OUT:
	    Tcl_DictObjPut(NULL, paramDesc, literals[LIT_DIRECTION], 
			   literals[LIT_INOUT]);
	    break;
	default:
	    break;
	}
	typeHashEntry =
	    Tcl_FindHashEntry(&(pidata->typeNumHash),
			      (const char*) (sdata->params[i].dataType));
	if (typeHashEntry != NULL) {
	    dataTypeName = (Tcl_Obj*) Tcl_GetHashValue(typeHashEntry);
	    Tcl_DictObjPut(NULL, paramDesc, literals[LIT_TYPE], dataTypeName);
	}
	Tcl_DictObjPut(NULL, paramDesc, literals[LIT_PRECISION],
		       Tcl_NewIntObj(sdata->params[i].precision));
	Tcl_DictObjPut(NULL, paramDesc, literals[LIT_SCALE],
		       Tcl_NewIntObj(sdata->params[i].scale));
	Tcl_DictObjPut(NULL, retVal, paramName, paramDesc);
    }
	
    Tcl_SetObjResult(interp, retVal);
    return TCL_OK;
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
    //TODO delete stmt handle
    if (sdata->columnNames != NULL) {
	Tcl_DecrRefCount(sdata->columnNames);
    }
    if (sdata->nativeSql != NULL) {
	Tcl_DecrRefCount(sdata->nativeSql);
    }
    if (sdata->params != NULL) {
	ckfree((char*)sdata->params);
    }
    if (sdata->ociStmtHp != NULL) {
        OCIHandleFree((dvoid *) sdata->ociStmtHp, OCI_HTYPE_STMT);
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
    ConnectionData* cdata;	/* The Oracle connection object's data */
    StatementData* sdata;	/* The statement object's data */
    ResultSetData* rdata;	/* THe result set object's data */

    ub2 bindLen;		/* Length of actual parameter */
    ub2* definedLengths;	/* Length of output fields */
    char** definedValues;	/* Array of output field values */
    ub2* definedIndicators;	/* Indicators of per-column errors */

    int nParams;		/* The parameter count on the statement */
    int nBound;			/* Number of parameters bound so far */
    int nDefined;   		/* Number of columns defined so far */
    OCIDefine* ociDefine;	/* Handle for defined spaceholder */ 
    OCIBind* ociBind;		/* Handle for bound parameter */
    Tcl_Obj* paramNameObj;	/* Name of the current parameter */
    const char* paramName;	/* Name of the current parameter */
    Tcl_Obj* paramValObj;	/* Value of the current parameter */
    char * paramValStr;	/* Representation of value of current parameter */
    int nColumns;		/* Number of columns in the result set */
    int stmtIters = 1;		/* Numer of statement execution iterations */
    int execMode = OCI_DEFAULT; /* Statement execution mode */
    ub2 stmtType;		/* Statement type */
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
			 " does not refer to a Oracle statement", NULL);
	return TCL_ERROR;
    }
    cdata = sdata->cdata;

    /* 
     * If there is no transaction in progress, turn on auto-commit so that
     * this statement will execute directly.
     */

    if ((cdata->flags & (CONN_FLAG_IN_XCN | CONN_FLAG_AUTOCOMMIT)) == 0) {
	cdata->flags |= CONN_FLAG_AUTOCOMMIT;
    }
    pidata = cdata->pidata;

    /* Allocate an object to hold data about this result set */

    rdata = (ResultSetData*) ckalloc(sizeof(ResultSetData));
    rdata->refCount = 1;
    rdata->sdata = sdata;
    rdata->ociStmtHp = NULL;
    rdata->definedValues = NULL; 
    rdata->definedIndicators = NULL;
    rdata->definedLengths = NULL;
    rdata->rowCount = 0; 
    rdata->badCursorState = 0;

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
	if (paramValObj != NULL) { 
	
	    /* 
	     * At this point, paramValObj contains the parameter to bind.
	     * Convert the parameters to the appropriate data types for
	     * Oracle's prepared statement interface, and bind them.
	     */
	    
	    paramValStr = Tcl_GetString(paramValObj);
	    bindLen = strlen(paramValStr) + 1;
	} else {
	    paramValStr = NULL;
	    bindLen = 0;
	}

	ociBind = NULL;
	/* Bind value */
	status = OCIBindByPos(rdata->ociStmtHp, &ociBind, cdata->ociErrHp,
	       nBound + 1, paramValStr , bindLen, SQLT_STR, NULL, NULL,
	       	NULL, 0 , NULL, OCI_DEFAULT);

	if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	    return TCL_ERROR;
	}
    }

    OCIAttrGet(rdata->ociStmtHp, OCI_HTYPE_STMT, &stmtType, NULL,
	    OCI_ATTR_STMT_TYPE, cdata->ociErrHp); 

    if (stmtType == OCI_STMT_SELECT) { 

	/* Select statements should not fetch any rows now,
	   so number of execution iterations is zeroed */

	stmtIters = 0; 
    }

    if (cdata->flags & CONN_FLAG_AUTOCOMMIT) {
	execMode |= OCI_COMMIT_ON_SUCCESS;
    }

    /* Execute the statement */
    status = OCIStmtExecute(cdata->ociSvcHp, rdata->ociStmtHp,
	    cdata->ociErrHp, stmtIters, 0, NULL, NULL, execMode);
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

    definedValues = rdata->definedValues = (char**)ckalloc(
	    nColumns * sizeof(char*));
    definedLengths = rdata->definedLengths = (ub2*) ckalloc(nColumns * sizeof(ub2));
    definedIndicators = rdata->definedIndicators = (ub2*) ckalloc(
	    nColumns * sizeof(ub2));

    for (nDefined = 0; nDefined < nColumns; nDefined += 1) { 
	int colSize; 
	OCIParam* ociParamH;
	definedValues[nDefined] = NULL; 

	OCIParamGet(rdata->ociStmtHp, OCI_HTYPE_STMT, 
		rdata->sdata->cdata->ociErrHp, (dvoid**)&ociParamH, nDefined + 1); 
	status = OCIAttrGet(ociParamH, OCI_DTYPE_PARAM, &colSize, 
		0, OCI_ATTR_DATA_SIZE, cdata->ociErrHp);
	
	if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {

	    OCIDescriptorFree(ociParamH, OCI_DTYPE_PARAM); 
	    goto freeRData;
	}
	
	OCIDescriptorFree(ociParamH, OCI_DTYPE_PARAM); 

	definedValues[nDefined] = ckalloc(colSize);

	ociDefine = NULL;
	
	status = OCIDefineByPos(rdata->ociStmtHp, &ociDefine,
		cdata->ociErrHp, nDefined + 1, definedValues[nDefined],
		colSize, SQLT_STR, &definedIndicators[nDefined],
		&definedLengths[nDefined], NULL, OCI_DEFAULT);
	if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {

	    OCIDescriptorFree(ociParamH, OCI_DTYPE_PARAM); 
	    goto freeRData;
	}

    }

    /* Determine and store row count */

    status = OCIAttrGet(rdata->ociStmtHp, OCI_HTYPE_STMT,
	    &rdata->rowCount, 0, OCI_ATTR_ROW_COUNT,  cdata->ociErrHp);  
    if (TransferOracleError(interp, cdata->ociErrHp, status) != TCL_OK) {
	goto freeRData;
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
 * ResultSetRowcountMethod --
 *
 *	Returns (if known) the number of rows affected by a Oracle statement.
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
    Tcl_SetObjResult(interp,
		     Tcl_NewWideIntObj((Tcl_WideInt)(rdata->rowCount)));
    return TCL_OK;
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

    if (rdata->definedLengths != NULL) {
        ckfree((char*)rdata->definedLengths);
    }

    if (rdata->definedIndicators != NULL) { 
	ckfree((char*) rdata->definedIndicators);
    }

    if (rdata->definedValues != NULL) {
	if (rdata->sdata->columnNames) { 
	    Tcl_ListObjLength(NULL, rdata->sdata->columnNames, &nColumns);
	    for (i=0; i<nColumns; i += 1) { 
		if (rdata->definedValues[i] == NULL) 
		    break;
		ckfree(rdata->definedValues[i]);
	    }
	    
	}
	ckfree((char*)rdata->definedValues);
    }

    if (rdata->ociStmtHp != NULL) {
	if (rdata->ociStmtHp != rdata->sdata->ociStmtHp) { 
	    OCIHandleFree((dvoid *) rdata->ociStmtHp, OCI_HTYPE_STMT);
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
    sword ociStatus;		/* Status returned by oci calls */
    int status = TCL_ERROR;	/* Status return from this command */
    int i;

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

    if (rdata->badCursorState == 1) {
	Tcl_SetObjResult(interp, literals[LIT_0]);
	return TCL_OK;
    }

    resultRow = Tcl_NewObj();
    Tcl_IncrRefCount(resultRow);
    
    ociStatus = OCIStmtFetch(sdata->ociStmtHp, cdata->ociErrHp, 1, OCI_FETCH_NEXT, OCI_DEFAULT);

    if (ociStatus == OCI_NO_DATA) {
	
        /* EOF was reached */

	rdata->badCursorState = 1;
	Tcl_SetObjResult(interp, literals[LIT_0]);
	return TCL_OK;

    } else {
	if (TransferOracleError(interp, cdata->ociErrHp, ociStatus) != TCL_OK ) {
	    printf("epic fail\n");
	    goto cleanup;
	}
    }

    /* Retrieve one column at a time. */
    for (i = 0; i < nColumns; ++i) {
	Tcl_ListObjIndex(NULL, sdata->columnNames, i, &colName);
	if (rdata->definedIndicators[i] == 0) {
	    colObj = Tcl_NewStringObj(rdata->definedValues[i], rdata->definedLengths[i]);
	} else { 
	    colObj = NULL; 
	}
	if (lists) {
	    if (colObj == NULL) {
		colObj = Tcl_NewObj();
	    }
	    Tcl_ListObjAppendElement(NULL, resultRow, colObj);
	} else { 
	    if (colObj != NULL) {
		Tcl_ListObjIndex(NULL, sdata->columnNames, i, &colName);
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
     * Initialize the Oracle library
     */
    OCIInitialize(OCI_OBJECT, NULL, NULL, NULL, NULL);
    
    (void) OCIEnvInit(&pidata->ociEnvHp, OCI_DEFAULT, 0, NULL);

    return TCL_OK;
}
