
#include <tcl.h>
#include <tclOO.h>
#include <tdbc.h>

#include <stdio.h>
#include <string.h>

#include <libpq-fe.h>


#define not_implemented fprintf(stderr,"%s,%s(): Not implemented (yet)\n", __FILE__,__FUNCTION__); return TCL_ERROR


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


/*
 * Structure that holds per-interpreter data for the POSTGRES package.
 */

typedef struct PerInterpData {
    int refCount;		    /* Reference count */
    Tcl_Obj* literals[LIT__END];    /* Literal pool */
    Tcl_HashTable typeNumHash;	    /* Lookup table for type numbers */
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
 * Structure that carries the data for a Postgres connection
 *
 * 	The 
 *	referring to it, which avoids taking down the TDBC until the
 *	other objects that refer to it vanish.
 */

typedef struct ConnectionData {
    int refCount;		/* Reference count. */
    PerInterpData* pidata;	/* Per-interpreter data */
    PGconn* pgPtr;		/* Postgres  connection handle */
    int stmtCounter;		/* Counter for naming statements */
//    int nCollations;		/* Number of collations defined */
//    int* collationSizes;	/* Character lengths indexed by collation ID */
//    int flags;
} ConnectionData;


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
 * Structure that carries the data for a Postgres prepared statement.
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
				 * Postgres */
    char* stmtName;		/* Name identyfing the statement */
    Tcl_Obj* columnNames;	/* Column names in the result set */
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
 * Structure describing a Postgres result set.  The object that the Tcl
 * API terms a "result set" actually has to be represented by a Postgres
 * "statement", since a Postgres statement can have only one set of results
 * at any given time.
 */

typedef struct ResultSetData {
    int refCount;		/* Reference count */
    StatementData* sdata;	/* Statement that generated this result set */
    PGresult* execResult;	/* Structure containing result of prepared statement execution */
    char* stmtName;		/* Name identyfing the statement */
    int rowCount;		/* Number of already retreived rows */
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




typedef struct PostgresDataType {
    const char* name;		/* Type name */
    int num;			/* Type number */
} PostgresDataType;
static const PostgresDataType dataTypes[] = {
    { "varchar",    0  },
    { NULL, 0 }
};


/* Configuration options for Postgres connections */

/* Data types of configuration options */

enum OptType {
    TYPE_STRING,		/* Arbitrary character string */
    TYPE_PORT			/* Port number */
};

/* Locations of the string options in the string array */
enum OptStringIndex {
    INDX_HOST, INDX_HOSTA, INDX_PORT, INDX_DB, INDX_USER,
    INDX_PASS, INDX_OPT, INDX_TTY, INDX_SERV,
    INDX_MAX
};

/* Names of string options for Postgres PGconnectdb() */
const char *  optStringNames[] = {
    "host", "hostaddr", "port", "dbname", "user",
    "password", "options", "tty", "service"
};

/* Flags in the configuration table */

#define CONN_OPT_FLAG_MOD 0x1	/* Configuration value changable at runtime */
#define CONN_OPT_FLAG_ALIAS 0x2	/* Configuration option is an alias */

 /* Table of configuration options */

static const struct {
    const char * name;		    /* Option name */
    enum OptType type;		    /* Option data type */
    int info;		    	    /* Option index or flag value */
    int flags;			    /* Flags - modifiable; SSL related; is an alias */
    char *(*queryF)(const PGconn*); /* Function used to determine the option value */
    const char* query;		    /* query if no queryF() given  */

} ConnOptions [] = {
// must-be: encoding, isolation, timeout, readonly

// from libpq:  connect_timeout, options,  sslmode, requiressl, krbsrvname, 
    { "-host",	    TYPE_STRING,    INDX_HOST,	0,			PQhost,	NULL},
    { "-hostaddr",  TYPE_STRING,    INDX_HOSTA,	0,			NULL,	NULL},
    { "-port",	    TYPE_PORT,      INDX_PORT,	0,			PQport,	NULL},
    { "-database",  TYPE_STRING,    INDX_DB,	0,			PQdb,	NULL},
    { "-db",	    TYPE_STRING,    INDX_DB,	CONN_OPT_FLAG_ALIAS,	PQdb,	NULL},
    { "-user",	    TYPE_STRING,    INDX_USER,	0,			PQuser,	NULL},
    { "-password",  TYPE_STRING,    INDX_PASS,	0,			PQpass, NULL},
    { "-options",   TYPE_STRING,    INDX_OPT,	0,			PQoptions,	NULL},
    { "-tty",	    TYPE_STRING,    INDX_TTY,	0,			PQtty,	NULL},
    { "-service",   TYPE_STRING,    INDX_SERV,	0,			NULL,	NULL},
    { NULL,	    0,		    0,		0,			NULL,	NULL}
};

static void TransferPostgresError(Tcl_Interp* interp, PGconn * pgPtr);
static int TransferResultError(Tcl_Interp* interp, PGresult * res);

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
static int ConnectionNeedCollationInfoMethod(ClientData clientData,
					     Tcl_Interp* interp,
					     Tcl_ObjectContext context,
					     int objc, Tcl_Obj *const objv[]);
static int ConnectionRollbackMethod(ClientData clientData, Tcl_Interp* interp,
				    Tcl_ObjectContext context,
				    int objc, Tcl_Obj *const objv[]);
static int ConnectionSetCollationInfoMethod(ClientData clientData,
					    Tcl_Interp* interp,
					    Tcl_ObjectContext context,
					    int objc, Tcl_Obj *const objv[]);
static int ConnectionTablesMethod(ClientData clientData, Tcl_Interp* interp,
				  Tcl_ObjectContext context,
				  int objc, Tcl_Obj *const objv[]);



static void DeleteConnectionMetadata(ClientData clientData);
static void DeleteConnection(ConnectionData* cdata);
static int CloneConnection(Tcl_Interp* interp, ClientData oldClientData,
			   ClientData* newClientData);

static char* GenStatementName(ConnectionData* cdata);
static StatementData* NewStatement(ConnectionData* cdata);
//TODO: is this function really allocating anything? maybe it's the right time to name it "PrepareStatement"?
static PGresult* AllocAndPrepareStatement(Tcl_Interp* interp,
					    StatementData* sdata, char* stmtName);

static Tcl_Obj* ResultDescToTcl(PGresult* resultDesc, int flags);


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

const static Tcl_MethodType ConnectionNeedCollationInfoMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "NeedCollationInfo",	/* name */
    ConnectionNeedCollationInfoMethod,	/* callProc */
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

const static Tcl_MethodType ConnectionSetCollationInfoMethodType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "SetCollationInfo",		/* name */
    ConnectionSetCollationInfoMethod,	/* callProc */
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
    &ConnectionNeedCollationInfoMethodType,
    &ConnectionRollbackMethodType,
    &ConnectionSetCollationInfoMethodType,
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




/* Initialization script */

static const char initScript[] =
    "namespace eval ::tdbc::postgres {}\n"
    "tcl_findLibrary tdbcpostgres " PACKAGE_VERSION " " PACKAGE_VERSION
    " tdbcpostgres.tcl TDBCPOSTGRES_LIBRARY ::tdbc::postgres::Library";

    
/*
 *-----------------------------------------------------------------------------
 *
 * TransferPostgresError --
 *
 *	Obtains the connection related error message from the Postgres
 *	client library and transfers them into the Tcl interpreter. 
 *	Unfortunately we cannot get error number or SQL state in 
 *	connection context. 
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Sets the interpreter result and error code to describe the SQL connection error.
 *
 *-----------------------------------------------------------------------------
 */

static void
TransferPostgresError(
    Tcl_Interp* interp,		/* Tcl interpreter */
    PGconn* pgPtr		/* Postgres connection handle */
) {

    //TODO generate PGResult * with PQmakeEmptyPGresult
    Tcl_Obj* errorCode = Tcl_NewObj();
    Tcl_ListObjAppendElement(NULL, errorCode, Tcl_NewStringObj("TDBC", -1));
    Tcl_ListObjAppendElement(NULL, errorCode,
			     Tcl_NewStringObj("GENERAL_ERROR", -1));
    Tcl_ListObjAppendElement(NULL, errorCode,
			     Tcl_NewStringObj("HY000", -1));
    Tcl_ListObjAppendElement(NULL, errorCode, Tcl_NewStringObj("POSTGRES", -1));
    Tcl_ListObjAppendElement(NULL, errorCode,
			     Tcl_NewIntObj(-1));
    Tcl_SetObjErrorCode(interp, errorCode);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(PQerrorMessage(pgPtr), -1));
}


/*
 *-----------------------------------------------------------------------------
 *
 * TransferPostgresError --
 *
 *	Check if there is any error related to given PGresult object. 
 *	If there was an error it obtainss error message, SQL state
 *	and error number from the Postgres clien library and transfers
 *	thenm into the Tcl interpreter. 
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

static int TransferResultError(
	Tcl_Interp* interp,
	PGresult * res
) {
    ExecStatusType error = PQresultStatus(res);
    const char* sqlstate;

    if (error == PGRES_EMPTY_QUERY || error == PGRES_BAD_RESPONSE ||
	    error == PGRES_NONFATAL_ERROR || error == PGRES_FATAL_ERROR) {
	Tcl_Obj* errorCode = Tcl_NewObj();
	Tcl_ListObjAppendElement(NULL, errorCode, Tcl_NewStringObj("TDBC", -1));

	sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	Tcl_ListObjAppendElement(NULL, errorCode,
		Tcl_NewStringObj(Tdbc_MapSqlState(sqlstate), -1));
	Tcl_ListObjAppendElement(NULL, errorCode,
		Tcl_NewStringObj(sqlstate, -1));
	Tcl_ListObjAppendElement(NULL, errorCode, Tcl_NewStringObj("POSTGRES", -1));
	Tcl_ListObjAppendElement(NULL, errorCode,
		Tcl_NewIntObj(error));
	Tcl_SetObjErrorCode(interp, errorCode);
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY),
		    -1));
    }
    if (error == PGRES_EMPTY_QUERY || error == PGRES_BAD_RESPONSE ||
	    error == PGRES_FATAL_ERROR) 
	return TCL_ERROR;
    else
	return TCL_OK; 
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
    char * value;		/* Return value as C string */

    if (ConnOptions[optionNum].queryF != NULL) {
	value = ConnOptions[optionNum].queryF(cdata->pgPtr);
	if (value == NULL) {
	    TransferPostgresError(interp, cdata->pgPtr);
	    return NULL; 
	} else 
	    return Tcl_NewStringObj(value, -1);
    }
    if (ConnOptions[optionNum].query != NULL){
        //TODO: SQL query, when no queryF given
	return NULL; 
    }
    
    if (ConnOptions[optionNum].type == TYPE_STRING &&
	    ConnOptions[optionNum].info != -1) {
	/* Fallback: try to get parameter value by generic function */
	value = (char*) PQparameterStatus(cdata->pgPtr,
		optStringNames[ConnOptions[optionNum].info]);
	if (value == NULL) {
	    TransferPostgresError(interp, cdata->pgPtr);
	    return NULL;
	} else
	    return Tcl_NewStringObj(value, -1);
    }
    return NULL; 
}


/*
 *-----------------------------------------------------------------------------
 *
 * ConfigureConnection --
 *
 *	Applies configuration settings to a Postrgre connection.
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
    int optionIndex;		/* Index of the current option in ConnOptions */
    int optionValue;		/* Integer value of the current option */
    int i,j;
    char portval[10];		/* String representation of port number */
    const char* stringOpts[INDX_MAX];
#define CONNINFO_LEN 1000
    char connInfo[CONNINFO_LEN];	/* COnfiguration string for PQconnectdb() */ 

    Tcl_Obj* retval;
    Tcl_Obj* optval;

    if (cdata->pgPtr != NULL) {

	/* Query configuration options on an existing connection */

	if (objc == skip) {
	    /* Return all options as a dict */
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
	    /* Return one option value */
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

    /* In all cases number of parameters must be even */
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

	if (cdata->pgPtr != NULL && !(ConnOptions[optionIndex].flags
					 & CONN_OPT_FLAG_MOD)) {
	    Tcl_Obj* msg = Tcl_NewStringObj("\"", -1);
	    Tcl_AppendObjToObj(msg, objv[i]);
	    Tcl_AppendToObj(msg, "\" option cannot be changed dynamically", -1);
	    Tcl_SetObjResult(interp, msg);
	    Tcl_SetErrorCode(interp, "TDBC", "GENERAL_ERROR", "HY000", 
			     "POSTGRES", "-1", NULL);
	    return TCL_ERROR;
	}

	/* Record option value */

	switch (ConnOptions[optionIndex].type) {
	case TYPE_STRING:
	    stringOpts[ConnOptions[optionIndex].info] =
		Tcl_GetString(objv[i+1]);
	    break;
/*	case TYPE_FLAG:
	    if (Tcl_GetBooleanFromObj(interp, objv[i+1], &optionValue)
		!= TCL_OK) {
		return TCL_ERROR;
	    }
	    if (optionValue) {
		postgresFlags |= ConnOptions[optionIndex].info;
	    }
	    break;
	case TYPE_ENCODING:
	    if (strcmp(Tcl_GetString(objv[i+1]), "utf-8")) {
		Tcl_SetObjResult(interp,
				 Tcl_NewStringObj("Only UTF-8 transfer "
						  "encoding is supported.\n",
						  -1));
		Tcl_SetErrorCode(interp, "TDBC", "GENERAL_ERROR", "HY000",
				 "POSTGRES", "-1", NULL);
		return TCL_ERROR;
	    }
	    break;
	case TYPE_ISOLATION:
	    if (Tcl_GetIndexFromObj(interp, objv[i+1], TclIsolationLevels,
				    "isolation level", TCL_EXACT, &isolation)
		!= TCL_OK) {
		return TCL_ERROR;
	    }
	    break;*/
	case TYPE_PORT:
	    if (Tcl_GetIntFromObj(interp, objv[i+1], &optionValue) != TCL_OK) {
		return TCL_ERROR;
	    }
	    if (optionValue < 0 || optionValue > 0xffff) {
		Tcl_SetObjResult(interp, Tcl_NewStringObj("port number must "
							  "be in range "
							  "[0..65535]", -1));
		Tcl_SetErrorCode(interp, "TDBC", "GENERAL_ERROR", "HY000",
				 "POSTGRES", "-1", NULL);
		return TCL_ERROR;
	    }
	    sprintf(portval, "%d", optionValue);
	    optStringNames[INDX_PORT] = portval;
	break;
/*	case TYPE_READONLY:
	    if (Tcl_GetBooleanFromObj(interp, objv[i+1], &optionValue)
		!= TCL_OK) {
		return TCL_ERROR;
	    }
	    if (optionValue != 0) {
		Tcl_SetObjResult(interp,
				 Tcl_NewStringObj("Postgres does not support "
						  "readonly connections", -1));
		Tcl_SetErrorCode(interp, "TDBC", "GENERAL_ERROR", "HY000",
				 "POSTGRES", "-1", NULL);
		return TCL_ERROR;
	    }
	    break;
	case TYPE_TIMEOUT:
	    if (Tcl_GetIntFromObj(interp, objv[i+1], &timeout) != TCL_OK) {
		return TCL_ERROR;
	    }
	    break; */
	}
/*	if (ConnOptions[optionIndex].flags & CONN_OPT_FLAG_SSL) {
	    sslFlag = 1;
	} */
    }



    if (cdata->pgPtr == NULL) {
	j=0; 
	connInfo[0] = '\0'; 
	for (i=0; i<INDX_MAX; i+=1) {
	    if (stringOpts[i] != NULL ) {
		//TODO escape values
		strncpy(&connInfo[j], optStringNames[i], CONNINFO_LEN - j);
		j+=strlen(optStringNames[i]);
		strncpy(&connInfo[j], " = '", CONNINFO_LEN - j);
		j+=strlen(" = '"); 
		strncpy(&connInfo[j], stringOpts[i], CONNINFO_LEN - j);
		j+=strlen(stringOpts[i]);
		strncpy(&connInfo[j], "' ", CONNINFO_LEN - j);
		j+=strlen("' ");
	    }
	}
	cdata->pgPtr = PQconnectdb(connInfo);
	if (cdata->pgPtr == NULL) {
	    Tcl_SetObjResult(interp,
			     Tcl_NewStringObj("PQconnectdb() failed, propably out of memory.", -1));
	    Tcl_SetErrorCode(interp, "TDBC", "GENERAL_ERROR", "HY001", 
			     "POSTGRES", "NULL", NULL);
	    return TCL_ERROR;
	}

	if (PQstatus(cdata->pgPtr) != CONNECTION_OK) { 
	    TransferPostgresError(interp, cdata->pgPtr);
	    return TCL_ERROR; 
	}

	/* Configuring a new connection. Open the database */
    }
    
    return TCL_OK;
}



/*
 *-----------------------------------------------------------------------------
 *
 * ConnectionConstructor --
 *
 *	Constructor for ::tdbc::postgres::connection, which represents a
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
				/* Per-interp data for the POSTGRES package */
    Tcl_Object thisObject = Tcl_ObjectContextObject(context);
				/* The current object */
    int skip = Tcl_ObjectContextSkippedArgs(context);
				/* The number of leading arguments to skip */
    ConnectionData* cdata;	/* Per-connection data */

    /* Hang client data on this connection */

    cdata = (ConnectionData*) ckalloc(sizeof(ConnectionData));
    cdata->refCount = 1;
    cdata->pidata = pidata;
    cdata->pgPtr = NULL;
    cdata->stmtCounter = 0;
//    cdata->nCollations = 0;
 //   cdata->collationSizes = NULL;
 //   cdata->flags = 0;
    IncrPerInterpRefCount(pidata);
    Tcl_ObjectSetMetadata(thisObject, &connectionDataType, (ClientData) cdata);
    
    /* Configure the connection */

    if (ConfigureConnection(cdata, interp, objc, objv, skip) != TCL_OK) {
	return TCL_ERROR;
    }

    return TCL_OK;

}

/*
 *-----------------------------------------------------------------------------
 *
 * ConnectionBegintransactionMethod --
 *
 *	Method that requests that following operations on an POSTGRES connection
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
    not_implemented;
    //looks like PGexec("BEGIN")
    //
//    Tcl_Object thisObject = Tcl_ObjectContextObject(objectContext);
				/* The current connection object */
 //   ConnectionData* cdata = (ConnectionData*)
//	Tcl_ObjectGetMetadata(thisObject, &connectionDataType);

    /* Check parameters */

 //   if (objc != 2) {
//	Tcl_WrongNumArgs(interp, 2, objv, "");
//	return TCL_ERROR;
//    }

//    /* Reject attempts at nested transactions */

//    if (cdata->flags & CONN_FLAG_IN_XCN) {
//	Tcl_SetObjResult(interp, Tcl_NewStringObj("POSTGRES does not support "
//						  "nested transactions", -1));
//	Tcl_SetErrorCode(interp, "TDBC", "GENERAL_ERROR", "HYC00",
//			 "POSTGRES", "-1", NULL);
//	return TCL_ERROR;
//    }
//   cdata->flags |= CONN_FLAG_IN_XCN;
    
//TODO check what about autocommit in POSTGRES
//    /* Turn off autocommit for the duration of the transaction */
//
//  if (cdata->flags & CONN_FLAG_AUTOCOMMIT) {
//	if (postgres_autocommit(cdata->postgresPtr, 0)) {
//	    TransferPostgresError(interp, cdata->postgresPtr);
//	    return TCL_ERROR;
//	}
//	cdata->flags &= ~CONN_FLAG_AUTOCOMMIT;
//   }

//    return TCL_OK;
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
    not_implemented;
    // Looks like PQExec("COMMIT");
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
    not_implemented;
    //SEEMS Like PQExec of SELECT * FROM ...
    //and then PQnfields x PQfname
}


/*
 *-----------------------------------------------------------------------------
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
 * ConnectionNeedCollationInfoMethod --
 *
 *	Internal method that determines whether the collation lengths
 *	are known yet.
 *
 * Usage:
 *	$connection NeedCollationInfo
 *
 * Parameters:
 *	None.
 *
 * Results:
 *	Returns a Boolean value.
 *
 *-----------------------------------------------------------------------------
 */

static int
ConnectionNeedCollationInfoMethod(
    ClientData clientData,	/* Completion type */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext objectContext, /* Object context */
    int objc,			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    not_implemented;
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
    not_implemented;
    //Looks like PQexec("ROLLBACK");
}

/*
 *-----------------------------------------------------------------------------
 *
 * ConnectionSetCollationInfoMethod --
 *
 *	Internal method that saves the character lengths of the collations
 *
 * Usage:
 *	$connection SetCollationInfo {collationNum size} ...
 *
 * Parameters:
 *	One or more pairs of collation number and character length,
 *	ordered in decreasing sequence by collation number.
 *
 * Results:
 *	None.
 *
 * The [$connection columns $table] method needs to know the sizes
 * of characters in a given column's collation and character set.
 * This information is available by querying INFORMATION_SCHEMA, which
 * is easier to do from Tcl than C. This method passes in the results.
 * 
 *-----------------------------------------------------------------------------
 */

static int
ConnectionSetCollationInfoMethod(
    ClientData clientData,	/* Completion type */
    Tcl_Interp* interp,		/* Tcl interpreter */
    Tcl_ObjectContext objectContext, /* Object context */
    int objc,			/* Parameter count */
    Tcl_Obj *const objv[]	/* Parameter vector */
) {
    not_implemented;
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
    //$tableList = pg_exec($dbconn, "select * from pg_tables");
    not_implemented;
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
//    if (cdata->collationSizes != NULL) {
//	ckfree((char*) cdata->collationSizes);
//    }
    if (cdata->pgPtr != NULL) {
	PQfinish(cdata->pgPtr);
    }
    DecrPerInterpRefCount(cdata->pidata);
    ckfree((char*) cdata);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CloneConnection --
 *
 *	Attempts to clone an Postgres connection's metadata.
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
		     Tcl_NewStringObj("Postgres connections are not clonable", -1));
    return TCL_ERROR;
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
 *	down POSTGRES when it is no longer required.
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
 *	Callback executed when any of the POSTGRES client methods is cloned.
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
 * GenStatementName --
 *
 *	Generates a unique name for a Postgre  statement
 *
 * Results:
 *	Null terminated, free-able,  string containg the name.
 *
 *-----------------------------------------------------------------------------
 */

static char*
GenStatementName(
    ConnectionData* cdata	/* Instance data for the connection */
) {
    char stmtName[30];
    cdata->stmtCounter += 1;
    snprintf(stmtName, 30, "statement%d", cdata->stmtCounter);
    return strdup(stmtName);
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
    sdata->columnNames = NULL;
    sdata->flags = 0;
    sdata->stmtName = GenStatementName(cdata);

    return sdata;
}

/*
 *-----------------------------------------------------------------------------
 *
 * AllocAndPrepareStatement --
 *
 *	Allocate space for a Postgres prepared statement, and prepare the
 *	statement. When stmtName equals to NULL, satement name is taken from sdata strucure.
 *
 * Results:
 *	Returns the Posgre result object if successeful, and NULL on failure.
 *
 * Side effects:
 *	Prepares the statement.
 *	Stores error message and error code in the interpreter on failure.
 *
 *-----------------------------------------------------------------------------
 */

static PGresult*
AllocAndPrepareStatement(
    Tcl_Interp* interp,		/* Tcl interpreter for error reporting */
    StatementData* sdata,	/* Statement data */
    char * stmtName		/* Overriding name of the statement */
) {
    ConnectionData* cdata = sdata->cdata;
				/* Connection data */
    const char* nativeSqlStr;	/* Native SQL statement to prepare */
    int nativeSqlLen;		/* Length of the statement */
    PGresult * res;		/* result of statement preparing*/


    if (stmtName == NULL) {
	stmtName = sdata->stmtName;
    }

    /* Prepare the statement */
	
    nativeSqlStr = Tcl_GetStringFromObj(sdata->nativeSql, &nativeSqlLen);
//TODO: delete ths printf.
//    printf("nativeSqlStr=%s\n", nativeSqlStr);
    res = PQprepare(cdata->pgPtr, stmtName, nativeSqlStr, 0, NULL);
    if (res == NULL) {
        TransferPostgresError(interp, cdata->pgPtr);
    }
    return res;
}

/*
 *-----------------------------------------------------------------------------
 *
 * ResultDescToTcl --
 *
 *	Converts a Postgres result description for return as a Tcl list.
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
    PGresult* result,		/* Result set description */
    int flags			/* Flags governing the conversion */
) {
    Tcl_Obj* retval = Tcl_NewObj();
    Tcl_HashTable names;	/* Hash table to resolve name collisions */
    char * fieldName; 
    Tcl_InitHashTable(&names, TCL_STRING_KEYS);
    if (result != NULL) {
	unsigned int fieldCount = PQnfields(result);
	unsigned int i;
	char numbuf[16];
	for (i = 0; i < fieldCount; ++i) {
	    fieldName = PQfname(result, i);
	    Tcl_Obj* nameObj = Tcl_NewStringObj(fieldName, -1);
	    Tcl_IncrRefCount(nameObj);
	    int new;
	    Tcl_HashEntry* entry =
		Tcl_CreateHashEntry(&names, fieldName, &new);
	    int count = 1;
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
	}
    }
    Tcl_DeleteHashTable(&names);
    return retval;
}


/*
 *-----------------------------------------------------------------------------
 *
 * StatementConstructor --
 *
 *	C-level initialization for the object representing an Postgres prepared
 *	statement.
 *
 * Usage:
 *	statement new connection statementText
 *	statement create name connection statementText
 *
 * Parameters:
 *      connection -- the Postgres connection object
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
    PGresult* res;		/* Temporary result of libpq calls */
    char tmpstr[30];		/* Temporary array for strings */
    int i,j;


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
			 " does not refer to a Postgres connection", NULL);
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
     * Rewrite the tokenized statement to Postgres syntax. Reject the
     * statement if it is actually multiple statements.
     */

    if (Tcl_ListObjGetElements(interp, tokens, &tokenc, &tokenv) != TCL_OK) {
	goto freeTokens;
    }
    nativeSql = Tcl_NewObj();
    Tcl_IncrRefCount(nativeSql);
    j=0;
    
    for (i = 0; i < tokenc; ++i) {
	tokenStr = Tcl_GetStringFromObj(tokenv[i], &tokenLen);
	
	switch (tokenStr[0]) {
	case '$':
	case ':':
	case '@':
	    j+=1;
	    snprintf(tmpstr, 30, "$%d", j);
	    Tcl_AppendToObj(nativeSql, tmpstr, -1);
	    Tcl_ListObjAppendElement(NULL, sdata->subVars, 
				     Tcl_NewStringObj(tokenStr+1, tokenLen-1));
	    break;

	case ';':
	    Tcl_SetObjResult(interp,
			     Tcl_NewStringObj("tdbc::postgres"
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

    res = AllocAndPrepareStatement(interp, sdata, NULL);
    if (res == NULL) 
	goto freeSData;
    
    if (TransferResultError(interp, res) != TCL_OK)
	goto freeSData;

    PQclear(res);

    Tcl_ListObjLength(NULL, sdata->subVars, &nParams);
    sdata->params = (ParamData*) ckalloc(nParams * sizeof(ParamData));
    for (i = 0; i < nParams; ++i) {
	sdata->params[i].flags = PARAM_IN;
	//TODO: when settled thoughts about types, fix it:
	sdata->params[i].dataType = 0;
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
 * StatementParamsMethod --
 *
 *	Lists the parameters in a Postgres statement.
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
 * StatementParamtypeMethod --
 *
 *	Defines a parameter type in a Postgres statement.
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
 * DeleteStatementMetadata, DeleteStatement --
 *
 *	Cleans up when a Postgres statement is no longer required.
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
    if (sdata->stmtName != NULL) {
	/* TODO: "Also, although there is no libpq function for
	 * deleting a prepared statement, the SQL DEALLOCATE
	 * statement can be used for that purpose. " */
	ckfree(sdata->stmtName);
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
 *	Attempts to clone a Postgres statement's metadata.
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
		     Tcl_NewStringObj("Postgres statements are not clonable", -1));
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
//    PerInterpData* pidata;	/* The per-interpreter data for this package */
    ConnectionData* cdata;	/* The Postgres connection object's data */

    StatementData* sdata;	/* The statement object's data */
    ResultSetData* rdata;	/* THe result set object's data */
    int nParams;		/* The parameter count on the statement */
    Tcl_Obj* paramNameObj;	/* Name of the current parameter */
    const char* paramName;	/* Name of the current parameter */
    Tcl_Obj* paramValObj;	/* Value of the current parameter */

    const char** paramValues;		/* Table of values */
    int* paramLengths;		/* Table of parameter lengths */
    PGresult* res;		/* Temporary result */
    int i;

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
			 " does not refer to a Postgres statement", NULL);
	return TCL_ERROR;
    }
    cdata = sdata->cdata;

//TODO autocommit- what?

    rdata = (ResultSetData*) ckalloc(sizeof(ResultSetData));
    rdata->refCount = 1;
    rdata->sdata = sdata;
    rdata->stmtName = NULL;
    rdata->execResult = NULL;
    rdata->rowCount = 0;
    IncrStatementRefCount(sdata);
    Tcl_ObjectSetMetadata(thisObject, &resultSetDataType, (ClientData) rdata);



    /*
     * Find a statement handle that we can use to execute the SQL code.
     * If the main statement handle associated with the statement
     * is idle, we can use it.  Otherwise, we have to allocate and
     * prepare a fresh one.
     */

    if (sdata->flags & STMT_FLAG_BUSY) {
	rdata->stmtName = GenStatementName(cdata);
	res = AllocAndPrepareStatement(interp, sdata, rdata->stmtName);
	if (res == NULL) {
	    return TCL_ERROR;
	}
	if (TransferResultError(interp, res) != TCL_OK) {
	    return TCL_ERROR;
	}
    } else {
	rdata->stmtName = sdata->stmtName;
	sdata->flags |= STMT_FLAG_BUSY;
    }

    Tcl_ListObjLength(NULL, sdata->subVars, &nParams);
    
    paramValues = (const char**) ckalloc(nParams * sizeof(char* ));
    paramLengths = (int*) ckalloc(nParams * sizeof(int*)); 

    for (i=0; i<nParams; i++) {
	Tcl_ListObjIndex(NULL, sdata->subVars, i, &paramNameObj);
	paramName = Tcl_GetString(paramNameObj);
	if (objc == skip+2) {
	    /* Param from a dictionary */

	    if (Tcl_DictObjGet(interp, objv[skip+1],
			       paramNameObj, &paramValObj) != TCL_OK) {
		goto freeParamTables;
	    }
	} else {
	    /* Param from a variable */

	    paramValObj = Tcl_GetVar2Ex(interp, paramName, NULL, 
					TCL_LEAVE_ERR_MSG);

	}
	/* At this point, paramValObj contains the parameter value */
	if (paramValObj != NULL) {
	    //TODO: add type handling case here
	    paramValues[i]=Tcl_GetStringFromObj(paramValObj, &paramLengths[i]);

	} else {
	    //TODO add some POSTGRES NULL type here
	    paramValues[i] = NULL;
	}


    }

    /* Execute the statement */

    rdata->execResult = PQexecPrepared(cdata->pgPtr, rdata->stmtName, 
	    nParams, paramValues, paramLengths, NULL, 0);
    if (TransferResultError(interp, rdata->execResult) != TCL_OK) {
	goto freeParamTables;
    }

    sdata->columnNames = ResultDescToTcl(rdata->execResult, 0);
    Tcl_IncrRefCount(sdata->columnNames);

    ckfree((char*)paramValues);
    ckfree((char*)paramLengths);
    return TCL_OK;
    
    /* On error, unwind all the resource allocations */
 freeParamTables:
    ckfree((char*)paramValues);
    ckfree((char*)paramLengths);
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

    int nColumns = 0;		/* Number of columns in the result set */
    Tcl_Obj* colObj;		/* Column obtained from the row */
    Tcl_Obj* colName;		/* Name of the current column */
    Tcl_Obj* resultRow;		/* Row of the result set under construction */

    int status = TCL_ERROR;	/* Status return from this command */

    char * buffer;		/* buffer containing field value */
    int buffSize;		/* size of buffer containing field value */
    int i; 
    
    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 2, objv, "varName");
	return TCL_ERROR;
    }

    /* Check if row counter haven't already rech the last row */
    if (rdata->rowCount >= PQntuples(rdata->execResult)) {
	Tcl_SetObjResult(interp, literals[LIT_0]);
	return TCL_OK;
    }

    /* Get the column names in the result set. */

    Tcl_ListObjLength(NULL, sdata->columnNames, &nColumns);
    if (nColumns == 0) {
	Tcl_SetObjResult(interp, literals[LIT_0]);
	return TCL_OK;
    }
 

    resultRow = Tcl_NewObj();
    Tcl_IncrRefCount(resultRow);

    /* Retrieve one column at a time. */
    for (i = 0; i < nColumns; ++i) {
	colObj = NULL; 
	if (PQgetisnull(rdata->execResult, rdata->rowCount, i) == 0) { 
	    buffSize = PQgetlength(rdata->execResult, rdata->rowCount, i); 
	    buffer = PQgetvalue(rdata->execResult, rdata->rowCount, i);

	    switch (PQfformat(rdata->execResult, i)) {
		case 0:	/* textual format */
		    colObj = Tcl_NewStringObj(buffer, buffSize);
		    break;
		default:	/* binary format */
		    colObj = Tcl_NewByteArrayObj((unsigned char*)buffer, buffSize);

	    }
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

    /* Advance to the next row */
    rdata->rowCount += 1; 

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
 * DeleteResultSetMetadata, DeleteResultSet --
 *
 *	Cleans up when a Postgres result set is no longer required.
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
    
    if (rdata->stmtName != NULL) {
	if (rdata->stmtName != sdata->stmtName) {
	/* TODO: "Also, although there is no libpq function for
	 * deleting a prepared statement, the SQL DEALLOCATE
	 * statement can be used for that purpose. " */
	    ckfree(rdata->stmtName);
	} else {
	    sdata->flags &= ~ STMT_FLAG_BUSY;
	}
    }
    if (rdata->execResult != NULL) { 
	PQclear(rdata->execResult);
    }
    DecrStatementRefCount(rdata->sdata);
    ckfree((char*)rdata);

}


/*
 *-----------------------------------------------------------------------------
 *
 * CloneResultSet --
 *
 *	Attempts to clone a PostreSQL result set's metadata.
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
		     Tcl_NewStringObj("Postgres result sets are not clonable",
				      -1));
    return TCL_ERROR;
}
    
/*
 *-----------------------------------------------------------------------------
 *
 * ResultSetRowcountMethod --
 *
 *	Returns (if known) the number of rows affected by a Postgres statement.
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
    char * nTuples;
    Tcl_Object thisObject = Tcl_ObjectContextObject(context);
				/* The current result set object */
    ResultSetData* rdata = (ResultSetData*)
	Tcl_ObjectGetMetadata(thisObject, &resultSetDataType);
				/* Data pertaining to the current result set */
    StatementData* sdata = rdata->sdata;
				/* The current statement */
    ConnectionData* cdata = sdata->cdata;
    PerInterpData* pidata = cdata->pidata; /* Per-interp data */
    Tcl_Obj** literals = pidata->literals; /* Literal pool */

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 2, objv, "");
	return TCL_ERROR;
    }

    nTuples = PQcmdTuples(rdata->execResult);
    if (strlen(nTuples) == 0) { 
	Tcl_SetObjResult(interp, literals[LIT_0]);
    } else {
	Tcl_SetObjResult(interp,
	    Tcl_NewStringObj(nTuples, -1));
    }
    return TCL_OK;
}


	
/*
 *-----------------------------------------------------------------------------
 *
 * Tdbcpostgres_Init --
 *
 *	Initializes the TDBC-POSTGRES bridge when this library is loaded.
 *
 * Side effects:
 *	Creates the ::tdbc::postgres namespace and the commands that reside in it.
 *	Initializes the POSTGRES environment.
 *
 *-----------------------------------------------------------------------------
 */

extern DLLEXPORT int
Tdbcpostgres_Init(
    Tcl_Interp* interp		/* Tcl interpreter */
) {

    PerInterpData* pidata;	/* Per-interpreter data for this package */
    Tcl_Obj* nameObj;		/* Name of a class or method being looked up */
    Tcl_Object curClassObject;  /* Tcl_Object representing the current class */
    Tcl_Class curClass;		/* Tcl_Class representing the current class */
    int i; 

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

    if (Tcl_PkgProvide(interp, "tdbc::postgres", PACKAGE_VERSION) == TCL_ERROR) {
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

    nameObj = Tcl_NewStringObj("::tdbc::postgres::connection", -1);
	Tcl_IncrRefCount(nameObj);
    if ((curClassObject = Tcl_GetObjectFromObj(interp, nameObj)) == NULL) {
	Tcl_DecrRefCount(nameObj);
	return TCL_ERROR;
    }
    Tcl_DecrRefCount(nameObj);
    curClass = Tcl_GetObjectAsClass(curClassObject);
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

    nameObj = Tcl_NewStringObj("::tdbc::postgres::statement", -1);
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

    nameObj = Tcl_NewStringObj("::tdbc::postgres::resultset", -1);
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


    return TCL_OK;
}


/*
*-----------------------------------------------------------------------------
*
* DeletePerInterpData --
*
*	Delete per-interpreter data when the POSTGRES package is finalized
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
   ckfree((char *) pidata);

}


