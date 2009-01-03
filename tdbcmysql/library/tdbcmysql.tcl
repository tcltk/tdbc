# tdbcmysql.tcl --
#
#	Class definitions and Tcl-level methods for the tdbc::mysql bridge.
#
# Copyright (c) 2008 by Kevin B. Kenny
# See the file "license.terms" for information on usage and redistribution
# of this file, and for a DISCLAIMER OF ALL WARRANTIES.
#
# RCS: @(#) $Id: tdbcmysql.tcl,v 1.47 2008/02/27 02:08:27 kennykb Exp $
#
#------------------------------------------------------------------------------

package require tdbc

::namespace eval ::tdbc::mysql {

    namespace export connection datasources drivers

}

#------------------------------------------------------------------------------
#
# tdbc::mysql::connection --
#
#	Class representing a connection to a database through MYSQL.
#
#-------------------------------------------------------------------------------

::oo::class create ::tdbc::mysql::connection {

    superclass ::tdbc::connection

    # The constructor takes the connection string as its argument
    # It sets up a namespace to hold the statements associated with
    # the connection, and then delegates to the 'init' method (written in C)
    # to do the actual work of attaching to the database.

    constructor args {
	next
	my variable statementClass
	set statementClass ::tdbc::mysql::statement
	my init {*}$args
    }

    # The 'columns' method returns a dictionary describing the tables
    # in the database

    method columns {table {pattern %}} {

	# To return correct lengths of CHARACTER and BINARY columns,
	# we need to know the maximum lengths of characters in each
	# collation. We cache this information only once, on the first
	# call to 'columns'.

	if {[my NeedCollationInfo]} {
	    my SetCollationInfo {*}[my allrows -as lists {
		SELECT coll.id, cs.maxlen
		FROM INFORMATION_SCHEMA.COLLATIONS coll,
		     INFORMATION_SCHEMA.CHARACTER_SETS cs
		WHERE cs.CHARACTER_SET_NAME = coll.CHARACTER_SET_NAME
		ORDER BY coll.id DESC
	    }]
	}

	return [my Columns $table $pattern]
    }

    # The 'prepareCall' method gives a portable interface to prepare
    # calls to stored procedures.  It delegates to 'prepare' to do the
    # actual work.

    method preparecall {call} {
	regexp {^[[:space:]]*(?:([A-Za-z_][A-Za-z_0-9]*)[[:space:]]*=)?(.*)} \
	    $call -> varName rest
	if {$varName eq {}} {
	    my prepare \\{$rest\\}
	} else {
	    my prepare \\{:$varName=$rest\\}
	}
    }

    # The 'init', 'begintransaction', 'commit, 'rollback', 'tables' 
    # 'NeedCollationInfo', 'SetCollationInfo', and 'Columns' methods 
    # are implemented in C.

}

#------------------------------------------------------------------------------
#
# tdbc::mysql::statement --
#
#	The class 'tdbc::mysql::statement' models one statement against a
#       database accessed through an MYSQL connection
#
#------------------------------------------------------------------------------

::oo::class create ::tdbc::mysql::statement {

    superclass ::tdbc::statement

    # The constructor accepts the handle to the connection and the SQL code
    # for the statement to prepare.  It creates a subordinate namespace to
    # hold the statement's active result sets, and then delegates to the
    # 'init' method, written in C, to do the actual work of preparing the
    # statement.

    constructor {connection sqlcode} {
	next
	my variable resultSetClass 
	set resultSetClass ::tdbc::mysql::resultset
	my init $connection $sqlcode
    }

    # Methods implemented in C:
    # init statement ?dictionary?  
    #     Does the heavy lifting for the constructor
    # params
    #     Returns descriptions of the parameters of a statement.
    # paramtype paramname ?direction? type ?precision ?scale??
    #     Declares the type of a parameter in the statement

}

#------------------------------------------------------------------------------
#
# tdbc::mysql::tablesStatement --
#
#	The class 'tdbc::mysql::tablesStatement' represents the special
#	statement that queries the tables in a database through an MYSQL
#	connection.
#
#------------------------------------------------------------------------------

oo::class create ::tdbc::mysql::tablesStatement {

    superclass ::tdbc::statement

    # The constructor accepts the handle to the connection and a pattern
    # to match table names.  It works in all ways like the constructor of
    # the 'statement' class except that its 'init' method sets up to enumerate
    # tables and not run a SQL query.

    constructor {connection pattern} {
	next
	variable resultSetClass ::tdbc::mysql::resultset
	my init $connection $pattern
    }

    # The C code contains a variant implementation of the 'init' method.

}

#------------------------------------------------------------------------------
#
# tdbc::mysql::columnsStatement --
#
#	The class 'tdbc::mysql::tablesStatement' represents the special
#	statement that queries the columns of a table or view
#	in a database through an MYSQL connection.
#
#------------------------------------------------------------------------------

oo::class create ::tdbc::mysql::columnsStatement {

    superclass ::tdbc::statement

    # The constructor accepts the handle to the connection, a table
    # name, and a pattern to match column names. It works in all ways
    # like the constructor of the 'statement' class except that its
    # 'init' method sets up to enumerate tables and not run a SQL
    # query.

    constructor {connection table pattern} {
	next
	variable resultSetClass ::tdbc::mysql::resultset
	my init $connection $table $pattern
    }

    # The C code contains a variant implementation of the 'init' method.

}

#------------------------------------------------------------------------------
#
# tdbc::mysql::typesStatement --
#
#	The class 'tdbc::mysql::typesStatement' represents the special
#	statement that queries the types available in a database through
#	an MYSQL connection.
#
#------------------------------------------------------------------------------


oo::class create ::tdbc::mysql::typesStatement {

    superclass ::tdbc::statement

    # The constructor accepts the handle to the connection, and
    # (optionally) a data type number. It works in all ways
    # like the constructor of the 'statement' class except that its
    # 'init' method sets up to enumerate types and not run a SQL
    # query.

    constructor {connection args} {
	next
	variable resultSetClass ::tdbc::mysql::resultset
	my init $connection {*}$args
    }

    # The C code contains a variant implementation of the 'init' method.

}

#------------------------------------------------------------------------------
#
# tdbc::mysql::resultset --
#
#	The class 'tdbc::mysql::resultset' models the result set that is
#	produced by executing a statement against an MYSQL database.
#
#------------------------------------------------------------------------------

::oo::class create ::tdbc::mysql::resultset {

    superclass ::tdbc::resultset

    # Constructor looks like
    #     tdbc::mysql::resultset create resultSetName statement ?dictionary?
    # It delegates to the 'init' method (written in C) to run the statement
    # and set up the result set. The call to [my init] is wrapped in [uplevel]
    # so that [my init] can access variables in the caller's scope.

    constructor {statement args} {
	next
	uplevel 1 [list {*}[namespace code {my init}] $statement {*}$args]
    }

    # Methods implemented in C include:

    # init statement ?dictionary?
    #     -- Executes the statement against the database, optionally providing
    #        a dictionary of substituted parameters (default is to get params
    #        from variables in the caller's scope).
    # columns
    #     -- Returns a list of the names of the columns in the result.
    # nextrow ?-as dicts|lists? ?--? variableName
    #     -- Stores the next row of the result set in the given variable in
    #        the caller's scope, either as a dictionary whose keys are 
    #        column names and whose values are column values, or else
    #        as a list of cells.
    # rowcount
    #     -- Returns a count of rows affected by the statement, or -1
    #        if the count of rows has not been determined.

}
