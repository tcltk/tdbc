#include <tcl.h>
#include <tclOO.h>
#include <tdbc.h>

#include <stdio.h>
#include <string.h>

#include <libpq-fe.h>

/*
 * Structure that holds per-interpreter data for the POSTGRE package.
 */

typedef struct PerInterpData {
    int refCount;		/* Reference count */
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
 * Structure that carries the data for a Postgre connection
 *
 * 	The ConnectionData structure is refcounted to simplify the
 *	destruction of statements associated with a connection.
 *	When a connection is destroyed, the subordinate namespace that
 *	contains its statements is taken down, destroying them. It's
 *	not safe to take down the ConnectionData until nothing is
 *	referring to it, which avoids taking down the TDBC until the
 *	other objects that refer to it vanish.
 */

typedef struct ConnectionData {
    int refCount;		/* Reference count. */
    PerInterpData* pidata;	/* Per-interpreter data */
    PGconn* pgPtr;		/* Postgre  connection handle */
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




/* Configuration options for Postgre connections */

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

/* Names of string options for Postgre PGconnectdb() */
const char *  OptStringNames[] = {
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

static void TransferPostgreError(Tcl_Interp* interp, PGconn * pgPtr);
static Tcl_Obj* QueryConnectionOption(ConnectionData* cdata, Tcl_Interp* interp,
				      int optionNum);

static int ConfigureConnection(ConnectionData* cdata, Tcl_Interp* interp,
			       int objc, Tcl_Obj *const objv[], int skip);
static int ConnectionConstructor(ClientData clientData, Tcl_Interp* interp,
				 Tcl_ObjectContext context,
				 int objc, Tcl_Obj *const objv[]);


static void DeleteConnectionMetadata(ClientData clientData);
static void DeleteConnection(ConnectionData* cdata);
static int CloneConnection(Tcl_Interp* interp, ClientData oldClientData,
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



const static Tcl_MethodType ConnectionConstructorType = {
    TCL_OO_METHOD_VERSION_CURRENT,
				/* version */
    "CONSTRUCTOR",		/* name */
    ConnectionConstructor,	/* callProc */
    DeleteCmd,			/* deleteProc */
    CloneCmd			/* cloneProc */
};


/* Initialization script */

static const char initScript[] =
    "namespace eval ::tdbc::postgre {}\n"
    "tcl_findLibrary tdbcpostgre " PACKAGE_VERSION " " PACKAGE_VERSION
    " tdbcpostgre.tcl TDBCPOSTGRE_LIBRARY ::tdbc::postgre::Library";

    
/*
 *-----------------------------------------------------------------------------
 *
 * TransferPostgreError --
 *
 *	Obtains the connection related error message from the Postgre
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
TransferPostgreError(
    Tcl_Interp* interp,		/* Tcl interpreter */
    PGconn* pgPtr		/* Postgre connection handle */
) {
    Tcl_Obj* errorCode = Tcl_NewObj();
    Tcl_ListObjAppendElement(NULL, errorCode, Tcl_NewStringObj("TDBC", -1));
    Tcl_ListObjAppendElement(NULL, errorCode,
			     Tcl_NewStringObj("GENERAL_ERROR", -1));
    Tcl_ListObjAppendElement(NULL, errorCode,
			     Tcl_NewStringObj("HY000", -1));
    Tcl_ListObjAppendElement(NULL, errorCode, Tcl_NewStringObj("POSTGRE", -1));
    Tcl_ListObjAppendElement(NULL, errorCode,
			     Tcl_NewIntObj(-1));
    Tcl_SetObjErrorCode(interp, errorCode);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(PQerrorMessage(pgPtr), -1));
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
    Tcl_Obj* retval;		/* Return value */

    if (ConnOptions[optionNum].queryF != NULL) {
	retval = Tcl_NewStringObj(
		ConnOptions[optionNum].queryF(cdata->pgPtr),-1);
	if (retval == NULL)
	    TransferPostgreError(interp, cdata->pgPtr);
	return retval;
    }
    if (ConnOptions[optionNum].query != NULL){
        //TODO: SQL query, when no queryF given
	return NULL; 
    }
    
    if (ConnOptions[optionNum].type == TYPE_STRING &&
	    ConnOptions[optionNum].info != -1) {
	/* Fallback: try to get parameter value by generic function */
	retval = Tcl_NewStringObj(
		PQparameterStatus(cdata->pgPtr,
		    OptStringNames[ConnOptions[optionNum].info]),
		-1);
	if (retval == NULL)
	    TransferPostgreError(interp, cdata->pgPtr);
	return retval;
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

    const char* OptStringNames[INDX_MAX];
				/* String-valued options */
    int optionIndex;		/* Index of the current option in ConnOptions */
    int optionValue;		/* Integer value of the current option */
    int i,j;
    char portval[10];		/* String representation of port number */
#define CONNINFO_LEN 1000
    char connInfo[CONNINFO_LEN];	/* COnfiguration string for PQconnectdb() */ 

    Tcl_Obj* retval;
    Tcl_Obj* optval;

    memset(OptStringNames, 0, INDX_MAX);

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
	OptStringNames[i] = NULL;
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
			     "POSTGRE", "-1", NULL);
	    return TCL_ERROR;
	}

	/* Record option value */

	switch (ConnOptions[optionIndex].type) {
	case TYPE_STRING:
	    OptStringNames[ConnOptions[optionIndex].info] =
		Tcl_GetString(objv[i+1]);
	    break;
/*	case TYPE_FLAG:
	    if (Tcl_GetBooleanFromObj(interp, objv[i+1], &optionValue)
		!= TCL_OK) {
		return TCL_ERROR;
	    }
	    if (optionValue) {
		mysqlFlags |= ConnOptions[optionIndex].info;
	    }
	    break;
	case TYPE_ENCODING:
	    if (strcmp(Tcl_GetString(objv[i+1]), "utf-8")) {
		Tcl_SetObjResult(interp,
				 Tcl_NewStringObj("Only UTF-8 transfer "
						  "encoding is supported.\n",
						  -1));
		Tcl_SetErrorCode(interp, "TDBC", "GENERAL_ERROR", "HY000",
				 "MYSQL", "-1", NULL);
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
				 "POSTGRE", "-1", NULL);
		return TCL_ERROR;
	    }
	    sprintf(portval, "%d", optionValue);
	    OptStringNames[INDX_PORT] = portval;
	break;
/*	case TYPE_READONLY:
	    if (Tcl_GetBooleanFromObj(interp, objv[i+1], &optionValue)
		!= TCL_OK) {
		return TCL_ERROR;
	    }
	    if (optionValue != 0) {
		Tcl_SetObjResult(interp,
				 Tcl_NewStringObj("Postgre does not support "
						  "readonly connections", -1));
		Tcl_SetErrorCode(interp, "TDBC", "GENERAL_ERROR", "HY000",
				 "MYSQL", "-1", NULL);
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



    if (cdata->pgPtr != NULL) {
	j=0; 
	connInfo[0] = '\0'; 
	for (i=0; i<INDX_MAX; i+=1) {
	    if (OptStringNames[i] != NULL ) {
		//TODO escape values
		strncpy(&connInfo[j], OptStringNames[i], CONNINFO_LEN - j);
		j+=strlen(OptStringNames[i]);
		strncpy(&connInfo[j], " = '", CONNINFO_LEN - j);
		j+=strlen(" = '"); 
		strncpy(&connInfo[j], OptStringNames[i], CONNINFO_LEN - j);
		j+=strlen(OptStringNames[i]);
		strncpy(&connInfo[j], "' ", CONNINFO_LEN - j);
		j+=strlen("' ");
	    }
	}
	printf("conninfo: |%s|\n", connInfo);
	cdata->pgPtr = PQconnectdb(connInfo);
	if (cdata->pgPtr == NULL) {
	    Tcl_SetObjResult(interp,
			     Tcl_NewStringObj("PQconnectdb() failed, propably out of memory.", -1));
	    Tcl_SetErrorCode(interp, "TDBC", "GENERAL_ERROR", "HY001", 
			     "POSTGRE", "NULL", NULL);
	    return TCL_ERROR;
	}

	if (PQstatus(cdata->pgPtr) == CONNECTION_BAD) { 
	    TransferPostgreError(interp, cdata->pgPtr);
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
 *	Constructor for ::tdbc::postgre::connection, which represents a
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
				/* Per-interp data for the POSTGRE package */
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
 *	Attempts to clone an Postgre connection's metadata.
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
		     Tcl_NewStringObj("Postgre connections are not clonable", -1));
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
 *	down MYSQL when it is no longer required.
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
 *	Callback executed when any of the POSTGRE client methods is cloned.
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
 * DeletePerInterpData --
 *
 *	Delete per-interpreter data when the MYSQL package is finalized
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
//    int i;

/*    Tcl_HashSearch search;
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
    } */
    ckfree((char *) pidata);

    /*
     * TODO: decrease thread refcount and mysql_thread_end if need be
     */
}



/*
 *-----------------------------------------------------------------------------
 *
 * Tdbcpostgre_Init --
 *
 *	Initializes the TDBC-POSTGRE bridge when this library is loaded.
 *
 * Side effects:
 *	Creates the ::tdbc::postgre namespace and the commands that reside in it.
 *	Initializes the POSTGRE environment.
 *
 *-----------------------------------------------------------------------------
 */

extern DLLEXPORT int
Tdbcpostgre_Init(
    Tcl_Interp* interp		/* Tcl interpreter */
) {

    PerInterpData* pidata;	/* Per-interpreter data for this package */
    Tcl_Obj* nameObj;		/* Name of a class or method being looked up */
    Tcl_Object curClassObject;  /* Tcl_Object representing the current class */
    Tcl_Class curClass;		/* Tcl_Class representing the current class */

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

    if (Tcl_PkgProvide(interp, "tdbc::postgre", PACKAGE_VERSION) == TCL_ERROR) {
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


    /* 
     * Find the connection class, and attach an 'init' method to it.
     */

    nameObj = Tcl_NewStringObj("::tdbc::postgre::connection", -1);
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
    return TCL_OK;
}

