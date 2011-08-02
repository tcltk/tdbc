
static inline void
MysqlSaveClientVersion(unsigned long ver)
{
    /* Intentionally does nothing */
}

static inline size_t
MysqlGetBindSize()
{
    return sizeof(MYSQL_BIND);
}

static inline MYSQL_BIND*
MysqlBindIndex(
    MYSQL_BIND* b,
    int i
) {
    return b + i;
}

static inline MYSQL_FIELD*
MysqlFieldIndex(
    MYSQL_FIELD* fields,
    int i)
{
    return fields + i;
}

static inline void*
MysqlBindAllocBuffer(
    MYSQL_BIND* b,		/* Pointer to a binding array */
    int i,			/* Index into the array */
    unsigned long len		/* Length of the buffer to allocate or 0 */
) {
    void* block = NULL;
    if (len != 0) {
	block = ckalloc(len);
    }
    b[i].buffer = block;
    b[i].buffer_length = len;
    return block;
}

static inline void
MysqlBindFreeBuffer(
    MYSQL_BIND* b,		/* Pointer to a binding array */
    int i			/* Index into the array */
) {
    if (b[i].buffer) {
	ckfree(b[i].buffer);
	b[i].buffer = NULL;
    }
    b[i].buffer_length = 0;
}

static inline void*
MysqlBindGetBuffer(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i			/* Index in the binding array */
) {
    return b[i].buffer;
}

static inline unsigned long
MysqlBindGetBufferLength(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i			/* Index in the binding array */
) {
    return b[i].buffer_length;
}

static inline enum enum_field_types
MysqlBindGetBufferType(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i			/* Index in the binding array */
) {
    return b[i].buffer_type;
}

static inline void 
MysqlBindSetBufferType(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i,			/* Index in the binding array */
    enum enum_field_types t	/* Buffer type to assign */
) {
    b[i].buffer_type = t;
}

static inline void 
MysqlBindSetLength(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i,			/* Index in the binding array */
    unsigned long* p		/* Length pointer to assign */
) {
    b[i].length = p;
}

static inline void 
MysqlBindSetIsNull(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i,			/* Index in the binding array */
    my_bool* p			/* "Is null" indicator pointer to assign */
) {
    b[i].is_null = p;
}

static inline void 
MysqlBindSetError(
    MYSQL_BIND* b, 		/* Binding array to alter */
    int i,			/* Index in the binding array */
    my_bool* p			/* Error indicator pointer to assign */
) {
    b[i].error = p;
}

/* vim: set ts=8 sw=4 sts=4 noet: */

