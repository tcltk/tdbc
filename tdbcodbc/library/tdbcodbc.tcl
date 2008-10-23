# tdbcodbc.tcl --
#
#	Class definitions and Tcl-level methods for the tdbc::odbc bridge.
#
# Copyright (c) 2008 by Kevin B. Kenny
# See the file "license.terms" for information on usage and redistribution
# of this file, and for a DISCLAIMER OF ALL WARRANTIES.
#
# RCS: @(#) $Id: tdbcodbc.tcl,v 1.47 2008/02/27 02:08:27 kennykb Exp $
#
#------------------------------------------------------------------------------

package require Tdbc 0.1

::namespace eval ::tdbc::odbc {

    namespace export connection

    # Data types that are predefined in ODBC

    variable sqltypes [dict create \
			   1 char \
			   2 numeric \
			   3 decimal \
			   4 integer \
			   5 smallint \
			   6 float \
			   7 real \
			   8 double \
			   9 datetime \
			   12 varchar \
			   91 date \
			   92 time \
			   93 timestamp \
			   -1 longvarchar \
			   -2 binary \
			   -3 varbinary \
			   -4 longvarbinary \
			   -5 bigint \
			   -6 tinyint \
			   -7 bit \
			   -8 wchar \
			   -9 wvarchar \
			   -10 wlongvarchar \
			   -11 guid]
}

#------------------------------------------------------------------------------
#
# tdbc::odbc::connection --
#
#	Class representing a connection to a database through ODBC.
#
#-------------------------------------------------------------------------------

::oo::class create ::tdbc::odbc::connection {

    superclass ::tdbc::connection

    # The constructor takes the connection string as its argument
    # It sets up a namespace to hold the statements associated with
    # the connection, and then delegates to the 'init' method (written in C)
    # to do the actual work of attaching to the database.

    constructor args {
	next
	my variable statementClass
	my variable typemap
	set typemap $::tdbc::odbc::sqltypes
	set statementClass ::tdbc::odbc::statement
	my init {*}$args
	set typesStmt [tdbc::odbc::typesStatement new [self]]
	$typesStmt foreach row {
	    set typeNum [dict get $row DATA_TYPE]
	    if {![dict exists $typemap $typeNum]} {
		dict set typemap $typeNum [string tolower \
					       [dict get $row TYPE_NAME]]
	    }
	}
	rename $typesStmt {}
    }

    # The 'tables' method returns a dictionary describing the tables
    # in the database

    method tables {{pattern %}} {
	my variable statementSeq
	set stmt [::tdbc::odbc::tablesStatement create \
		      Stmt::[incr statementSeq] [self] $pattern]
       	set status [catch {
	    set retval {}
	    $stmt foreach -as dicts row {
		if {[dict exists $row TABLE_NAME]} {
		    dict set retval [dict get $row TABLE_NAME] $row
		}
	    }
	    set retval
	} result options]
	catch {rename stmt {}}
	return -level 0 -options $options $result
    }

    # The 'columns' method returns a dictionary describing the tables
    # in the database

    method columns {table {pattern %}} {
	my variable typemap
	my variable statementSeq
	set stmt [::tdbc::odbc::columnsStatement create \
		      Stmt::[incr statementSeq] [self] $table $pattern]
	set status [catch {
	    set retval {}
	    $stmt foreach -as dicts origrow {
		set row {}
		dict for {key value} $origrow {
		    dict set row [string tolower $key] $value
		}
		if {[dict exists $row column_name]} {
		    if {[dict exists $typemap \
			     [dict get $row data_type]]} {
			dict set row type \
			    [dict get $typemap \
				 [dict get $row data_type]]
		    } else {
			dict set row type [dict get $row type_name]
		    }
		    if {[dict exists $row column_size]} {
			dict set row precision \
			    [dict get $row column_size]
		    }
		    if {[dict exists $row decimal_digits]} {
			dict set row scale \
			    [dict get $row decimal_digits]
		    }
		    dict set row nullable \
			[expr {!![dict get $row is_nullable]}]
		    dict set retval [dict get $row column_name] $row
		}
	    }
	    set retval
	} result options]
	catch {rename stmt {}}
	return -level 0 -options $options $result
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

    # The 'TypeMap' method returns the type map

    method typemap {} {
	my variable typemap
	return $typemap
    }

    # The 'init', 'begintransaction', 'commit' and 'rollback' methods 
    # are implemented in C.

}

#-------------------------------------------------------------------------------
#
# tdbc::odbc::statement --
#
#	The class 'tdbc::odbc::statement' models one statement against a
#       database accessed through an ODBC connection
#
#-------------------------------------------------------------------------------

::oo::class create ::tdbc::odbc::statement {

    superclass ::tdbc::statement

    # The constructor accepts the handle to the connection and the SQL code
    # for the statement to prepare.  It creates a subordinate namespace to
    # hold the statement's active result sets, and then delegates to the
    # 'init' method, written in C, to do the actual work of preparing the
    # statement.

    constructor {connection sqlcode} {
	next
	my variable resultSetClass 
	set resultSetClass ::tdbc::odbc::resultset
	my init $connection $sqlcode
	my variable typemap
	set typemap [$connection typemap]
    }

    # The 'params' method describes the parameters to the statement

    method params {} {
	my variable typemap
	set result {}
	foreach {name flags typeNum precision scale nullable} [my ParamList] {
	    set lst [dict create \
			 name $name \
			 direction [lindex {unknown in out inout} \
					[expr {($flags & 0x06) >> 1}]] \
			 type [dict get $typemap $typeNum] \
			 precision $precision \
			 scale $scale]
	    if {$nullable in {0 1}} {
		dict set list nullable $nullable
	    }
	    dict set result $name $lst
	}
	return $result
    }

    # Methods implemented in C:
    # init statement ?dictionary?  
    #     Does the heavy lifting for the constructor
    # paramtype paramname ?direction? type ?precision ?scale??
    #     Declares the type of a parameter in the statement

}

#------------------------------------------------------------------------------
#
# tdbc::odbc::tablesStatement --
#
#	The class 'tdbc::odbc::tablesStatement' represents the special
#	statement that queries the tables in a database through an ODBC
#	connection.
#
#------------------------------------------------------------------------------

oo::class create ::tdbc::odbc::tablesStatement {

    superclass ::tdbc::statement

    # The constructor accepts the handle to the connection and a pattern
    # to match table names.  It works in all ways like the constructor of
    # the 'statement' class except that its 'init' method sets up to enumerate
    # tables and not run a SQL query.

    constructor {connection pattern} {
	next
	variable resultSetClass ::tdbc::odbc::resultset
	my init $connection $pattern
    }

    # The C code contains a variant implementation of the 'init' method.

}

#------------------------------------------------------------------------------
#
# tdbc::odbc::columnsStatement --
#
#	The class 'tdbc::odbc::tablesStatement' represents the special
#	statement that queries the columns of a table or view
#	in a database through an ODBC connection.
#
#------------------------------------------------------------------------------

oo::class create ::tdbc::odbc::columnsStatement {

    superclass ::tdbc::statement

    # The constructor accepts the handle to the connection, a table
    # name, and a pattern to match column names. It works in all ways
    # like the constructor of the 'statement' class except that its
    # 'init' method sets up to enumerate tables and not run a SQL
    # query.

    constructor {connection table pattern} {
	next
	variable resultSetClass ::tdbc::odbc::resultset
	my init $connection $table $pattern
    }

    # The C code contains a variant implementation of the 'init' method.

}

#------------------------------------------------------------------------------
#
# tdbc::odbc::typesStatement --
#
#	The class 'tdbc::odbc::typesStatement' represents the special
#	statement that queries the types available in a database through
#	an ODBC connection.
#
#------------------------------------------------------------------------------


oo::class create ::tdbc::odbc::typesStatement {

    superclass ::tdbc::statement

    # The constructor accepts the handle to the connection, and
    # (optionally) a data type number. It works in all ways
    # like the constructor of the 'statement' class except that its
    # 'init' method sets up to enumerate types and not run a SQL
    # query.

    constructor {connection args} {
	next
	variable resultSetClass ::tdbc::odbc::resultset
	my init $connection {*}$args
    }

    # The C code contains a variant implementation of the 'init' method.

}

#------------------------------------------------------------------------------
#
# tdbc::odbc::resultset --
#
#	The class 'tdbc::odbc::resultset' models the result set that is
#	produced by executing a statement against an ODBC database.
#
#------------------------------------------------------------------------------

::oo::class create ::tdbc::odbc::resultset {

    superclass ::tdbc::resultset

    # Constructor looks like
    #     tdbc::odbc::resultset create resultSetName statement ?dictionary?
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
