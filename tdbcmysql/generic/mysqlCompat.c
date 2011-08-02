
static unsigned long mysqlClientVersion; /* Version number of MySQL */

/*
 *-----------------------------------------------------------------------------
 *
 * MysqlSaveClientVersion --
 *
 *	Store version number of the client library for future use
 *	by the functions in this module
 *
 *-----------------------------------------------------------------------------
 */

static void
MysqlSaveClientVersion(unsigned long ver)
{
    mysqlClientVersion = ver;
}

/*
 *-----------------------------------------------------------------------------
 *
 * MysqlGetBindSize --
 *
 *	Returns the size of an instance of a MYSQL_BIND object
 *	depending on the version of the client library loaded
 *
 *-----------------------------------------------------------------------------
 */

static size_t
MysqlGetBindSize()
{
    if (mysqlClientVersion >= 50100) {
	return sizeof(struct st_mysql_bind_51);
    } else {
	return sizeof(struct st_mysql_bind_50);
    }
}

/*
 *-----------------------------------------------------------------------------
 *
 * MysqlBindIndex --
 *
 *	Returns a pointer to one of an array of MYSQL_BIND objects
 *
 *-----------------------------------------------------------------------------
 */

static MYSQL_BIND*
MysqlBindIndex(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i			/* Index in the binding array */
) {
    if (mysqlClientVersion >= 50100) {
	return (MYSQL_BIND*)(((struct st_mysql_bind_51*) b) + i);
    } else {
	return (MYSQL_BIND*)(((struct st_mysql_bind_50*) b) + i);
    }
}

/*
 *-----------------------------------------------------------------------------
 *
 * MysqlFieldIndex --
 *
 *	Return a pointer to a given MYSQL_FIELD structure in an array
 *
 * The MYSQL_FIELD structure grows by one pointer between 5.0 and 5.1.
 * Our code never creates a MYSQL_FIELD, nor does it try to access that
 * pointer, so we handle things simply by casting the types.
 *
 *-----------------------------------------------------------------------------
 */

static MYSQL_FIELD*
MysqlFieldIndex(MYSQL_FIELD* fields,
				/*  Pointer to the array*/
		int i)		/* Index in the array */
{
    MYSQL_FIELD* retval;
    if (mysqlClientVersion >= 50100) {
	retval = (MYSQL_FIELD*)(((struct st_mysql_field_51*) fields)+i);
    } else {
	retval = (MYSQL_FIELD*)(((struct st_mysql_field_50*) fields)+i);
    }
    return retval;
}

/* 
 *-----------------------------------------------------------------------------
 *
 * MysqlBindAllocBuffer --
 *
 *	Allocates the buffer in a MYSQL_BIND object
 *
 * Results:
 *	Returns a pointer to the allocated buffer
 *
 *-----------------------------------------------------------------------------
 */

static void*
MysqlBindAllocBuffer(
    MYSQL_BIND* b,		/* Pointer to a binding array */
    int i,			/* Index into the array */
    unsigned long len		/* Length of the buffer to allocate or 0 */
) {
    void* block = NULL;
    if (len != 0) {
	block = ckalloc(len);
    }
    if (mysqlClientVersion >= 50100) {
	((struct st_mysql_bind_51*) b)[i].buffer = block;
	((struct st_mysql_bind_51*) b)[i].buffer_length = len;
    } else {
	((struct st_mysql_bind_50*) b)[i].buffer = block;
	((struct st_mysql_bind_50*) b)[i].buffer_length = len;
    }
    return block;
}

/*
 *-----------------------------------------------------------------------------
 *
 * MysqlBindFreeBuffer --
 *
 *	Frees trhe buffer in a MYSQL_BIND object
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Buffer is returned to the system.
 *
 *-----------------------------------------------------------------------------
 */
static void
MysqlBindFreeBuffer(
    MYSQL_BIND* b,		/* Pointer to a binding array */
    int i			/* Index into the array */
) {
    if (mysqlClientVersion >= 50100) {
	struct st_mysql_bind_51* bindings = (struct st_mysql_bind_51*) b;
	if (bindings[i].buffer) {
	    ckfree(bindings[i].buffer);
	    bindings[i].buffer = NULL;
	}
	bindings[i].buffer_length = 0;
    } else {
	struct st_mysql_bind_50* bindings = (struct st_mysql_bind_50*) b;
	if (bindings[i].buffer) {
	    ckfree(bindings[i].buffer);
	    bindings[i].buffer = NULL;
	}
	bindings[i].buffer_length = 0;
    }
}

/*
 *-----------------------------------------------------------------------------
 *
 * MysqlBindGetBufferLength, MysqlBindSetBufferType, MysqlBindGetBufferType,
 * MysqlBindSetLength, MysqlBindSetIsNull,
 * MysqlBindSetError --
 *
 *	Access the fields of a MYSQL_BIND object
 *
 *-----------------------------------------------------------------------------
 */

static void*
MysqlBindGetBuffer(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i			/* Index in the binding array */
) {
    if (mysqlClientVersion >= 50100) {
	return ((struct st_mysql_bind_51*) b)[i].buffer;
    } else {
	return ((struct st_mysql_bind_50*) b)[i].buffer;
    }
}

static unsigned long
MysqlBindGetBufferLength(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i			/* Index in the binding array */
) {
    if (mysqlClientVersion >= 50100) {
	return ((struct st_mysql_bind_51*) b)[i].buffer_length;
    } else {
	return ((struct st_mysql_bind_50*) b)[i].buffer_length;
    }

}

static enum enum_field_types
MysqlBindGetBufferType(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i			/* Index in the binding array */
) {
    if (mysqlClientVersion >= 50100) {
	return ((struct st_mysql_bind_51*) b)[i].buffer_type;
    } else {
	return ((struct st_mysql_bind_50*) b)[i].buffer_type;
    }
}

static void 
MysqlBindSetBufferType(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i,			/* Index in the binding array */
    enum enum_field_types t	/* Buffer type to assign */
) {
    if (mysqlClientVersion >= 50100) {
	((struct st_mysql_bind_51*) b)[i].buffer_type = t;
    } else {
	((struct st_mysql_bind_50*) b)[i].buffer_type = t;
    }
}

static void 
MysqlBindSetLength(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i,			/* Index in the binding array */
    unsigned long* p		/* Length pointer to assign */
) {
    if (mysqlClientVersion >= 50100) {
	((struct st_mysql_bind_51*) b)[i].length = p;
    } else {
	((struct st_mysql_bind_50*) b)[i].length = p;
    }
}

static void 
MysqlBindSetIsNull(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i,			/* Index in the binding array */
    my_bool* p			/* "Is null" indicator pointer to assign */
) {
    if (mysqlClientVersion >= 50100) {
	((struct st_mysql_bind_51*) b)[i].is_null = p;
    } else {
	((struct st_mysql_bind_50*) b)[i].is_null = p;
    }
}

static void 
MysqlBindSetError(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i,			/* Index in the binding array */
    my_bool* p			/* Error indicator pointer to assign */
) {
    if (mysqlClientVersion >= 50100) {
	((struct st_mysql_bind_51*) b)[i].error = p;
    } else {
	((struct st_mysql_bind_50*) b)[i].error = p;
    }
}

/* vim: set ts=8 sw=4 sts=4 noet: */

