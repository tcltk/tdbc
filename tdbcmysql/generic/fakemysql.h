/*
 * fakemysql.h --
 *
 *	Fake definitions of the MySQL API sufficient to build tdbc::mysql
 *	without having an MySQL installation on the build system. This file
 *	comprises only data type, constant and function definitions.
 *
 * The programmers of this file believe that it contains material not
 * subject to copyright under the doctrines of scenes a faire and
 * of merger of idea and expression. Accordingly, this file is in the
 * public domain.
 *
 *-----------------------------------------------------------------------------
 */

#ifndef FAKEMYSQL_H_INCLUDED
#define FAKEMYSQL_H_INCLUDED

#include <stddef.h>

#ifndef MODULE_SCOPE
#define MODULE_SCOPE extern
#endif

MODULE_SCOPE Tcl_LoadHandle MysqlInitStubs(Tcl_Interp*);

#ifdef _WIN32
#define STDCALL __stdcall
#else
#define STDCALL /* nothing */
#endif

enum enum_field_types {
    MYSQL_TYPE_DECIMAL=0,
    MYSQL_TYPE_TINY=1,
    MYSQL_TYPE_SHORT=2,
    MYSQL_TYPE_LONG=3,
    MYSQL_TYPE_FLOAT=4,
    MYSQL_TYPE_DOUBLE=5,
    MYSQL_TYPE_NULL=6,
    MYSQL_TYPE_TIMESTAMP=7,
    MYSQL_TYPE_LONGLONG=8,
    MYSQL_TYPE_INT24=9,
    MYSQL_TYPE_DATE=10,
    MYSQL_TYPE_TIME=11,
    MYSQL_TYPE_DATETIME=12,
    MYSQL_TYPE_YEAR=13,
    MYSQL_TYPE_NEWDATE=14,
    MYSQL_TYPE_VARCHAR=15,
    MYSQL_TYPE_BIT=16,
    MYSQL_TYPE_NEWDECIMAL=246,
    MYSQL_TYPE_ENUM=247,
    MYSQL_TYPE_SET=248,
    MYSQL_TYPE_TINY_BLOB=249,
    MYSQL_TYPE_MEDIUM_BLOB=250,
    MYSQL_TYPE_LONG_BLOB=251,
    MYSQL_TYPE_BLOB=252,
    MYSQL_TYPE_VAR_STRING=253,
    MYSQL_TYPE_STRING=254,
    MYSQL_TYPE_GEOMETRY=255
};

enum mysql_option {
    MYSQL_SET_CHARSET_NAME=7,
};

#define CLIENT_COMPRESS		32
#define CLIENT_INTERACTIVE	1024	/* This is an interactive client */

#define MYSQL_NO_DATA 100
#define MYSQL_DATA_TRUNCATED 101

typedef struct st_mysql MYSQL;
typedef struct st_mysql_bind MYSQL_BIND;
typedef struct st_mysql_field MYSQL_FIELD;
typedef struct st_mysql_res MYSQL_RES;
typedef char** MYSQL_ROW;
typedef struct st_mysql_stmt MYSQL_STMT;

typedef char my_bool;
typedef Tcl_WideUInt my_ulonglong;

typedef struct st_net NET;

/* 
 * There are different version of the MYSQL_BIND structure before and after
 * MySQL 5.1. We go after the fields of the structure using accessor functions
 * so that the code in this file is compatible with both versions.
 */

struct st_mysql_bind_51 {	/* Post-5.1 */
    unsigned long* length;
    my_bool* is_null;
    void* buffer;
    my_bool* error;
    unsigned char* row_ptr;
    void (*store_param_func)(NET* net, MYSQL_BIND* param);
    void (*fetch_result)(MYSQL_BIND*, MYSQL_FIELD*, unsigned char**);
    void (*skip_result)(MYSQL_BIND*, MYSQL_FIELD*, unsigned char**);
    unsigned long buffer_length;
    unsigned long offset;
    unsigned long length_value;
    unsigned int param_number;
    unsigned int pack_length;
    enum enum_field_types buffer_type;
    my_bool error_value;
    my_bool is_unsigned;
    my_bool long_data_used;
    my_bool is_null_value;
    void* extension;
};

struct st_mysql_bind_50 {	/* Pre-5.1 */
    unsigned long* length;
    my_bool* is_null;
    void* buffer;
    my_bool* error;
    enum enum_field_types buffer_type;
    unsigned long buffer_length;
    unsigned char* row_ptr;
    unsigned long offset;
    unsigned long length_value;
    unsigned int param_number;
    unsigned int pack_length;
    my_bool error_value;
    my_bool is_unsigned;
    my_bool long_data_used;
    my_bool is_null_value;
    void (*store_param_func)(NET* net, MYSQL_BIND* param);
    void (*fetch_result)(MYSQL_BIND*, MYSQL_FIELD*, unsigned char**);
    void (*skip_result)(MYSQL_BIND*, MYSQL_FIELD*, unsigned char**);
};

/* 
 * There are also different versions of the MYSQL_FIELD structure; fortunately,
 * the 5.1 version is a strict extension of the 5.0 version.
 */

struct st_mysql_field {
    char* name;
    char *org_name;
    char* table;
    char* org_tabkle;
    char* db;
    char* catalog;
    char* def;
    unsigned long length;
    unsigned long max_length;
    unsigned int name_length;
    unsigned int org_name_length;
    unsigned int table_length;
    unsigned int org_table_length;
    unsigned int db_length;
    unsigned int catalog_length;
    unsigned int def_length;
    unsigned int flags;
    unsigned int decimals;
    unsigned int charsetnr;
    enum enum_field_types type;
};
struct st_mysql_field_50 {
    struct st_mysql_field field;
};
struct st_mysql_field_51 {
    struct st_mysql_field field;
    void* extension;
};
#define NOT_NULL_FLAG 1

#define IS_NUM(t)	((t) <= MYSQL_TYPE_INT24 || (t) == MYSQL_TYPE_YEAR || (t) == MYSQL_TYPE_NEWDECIMAL)

#define mysql_library_init mysql_server_init
#define mysql_library_end mysql_server_end

#include "mysqlStubs.h"

#endif /* not FAKEMYSQL_H_INCLUDED */
