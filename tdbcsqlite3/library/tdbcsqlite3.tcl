# tdbcsqlite3.tcl --
#
#    SQLite3 database driver for TDBC
#
# Copyright (c) 2008 by Kevin B. Kenny.
# See the file "license.terms" for information on usage and redistribution
# of this file, and for a DISCLAIMER OF ALL WARRANTIES.
#
# RCS: @(#) $Id: tdbcodbc.tcl,v 1.47 2008/02/27 02:08:27 kennykb Exp $
#
#------------------------------------------------------------------------------

package require tdbc
package require sqlite3

package provide tdbc::sqlite3 1.0b7

namespace eval tdbc::sqlite3 {
    namespace export connection
}

#------------------------------------------------------------------------------
#
# tdbc::sqlite3::connection --
#
#	Class representing a SQLite3 database connection
#
#------------------------------------------------------------------------------

::oo::class create ::tdbc::sqlite3::connection {

    superclass ::tdbc::connection

    # The constructor accepts a database name and opens the database.

    constructor {databaseName args} {
	my variable statementClass
	my variable timeout
	set statementClass ::tdbc::sqlite3::statement
	set timeout 0
	if {[llength $args] % 2 != 0} {
	    set cmd [lrange [info level 0] 0 end-[llength $args]]
	    return -code error "wrong # args, should be\
                                \"$cmd ?-option value?...\"" \
		-errorcode {TDBC GENERAL_ERROR HY000 SQLITE3 WRONGNUMARGS}
	}
	next
	sqlite3 [namespace current]::db $databaseName
	if {[llength $args] > 0} {
	    my configure {*}$args
	}
	db nullvalue \ufffd
    }

    # The 'configure' method queries and sets options to the database

    method configure args {
	my variable timeout
	if {[llength $args] == 0} {

	    # Query all configuration options

	    set result {-encoding utf-8}
	    lappend result -isolation
	    if {[db onecolumn {PRAGMA read_uncommitted}]} {
		lappend result readuncommitted
	    } else {
		lappend result serializable
	    }
	    lappend result -readonly 0 
	    lappend result -timeout $timeout
	    return $result

	} elseif {[llength $args] == 1} {

	    # Query a single option

	    set option [lindex $args 0]
	    switch -exact -- $option {
		-e - -en - -enc - -enco - -encod - -encodi - -encodin - 
		-encoding {
		    return utf-8
		}
		-i - -is - -iso - -isol - -isola - -isolat - -isolati -
		-isolatio - -isolation {
		    if {[db onecolumn {PRAGMA read_uncommitted}]} {
			return readuncommitted
		    } else {
			return serializable
		    }
		}
		-r - -re - -rea - -read - -reado - -readon - -readonl -
		-readonly {
		    return 0
		}
		-t - -ti - -tim - -time - -timeo - -timeou - -timeout {
		    return $timeout
		}
		default {
		    return -code error "bad option \"$option\": must be\
                                        -encoding, -isolation, -readonly\
                                        or -timeout" \
			-errorcode [list TDBC GENERAL_ERROR HY000 SQLITE3 \
					BADOPTION $option]
		    
		}
	    }

	} elseif {[llength $args] % 2 != 0} {

	    # Syntax error

	    set cmd [lrange [info level 0] 0 end-[llength $args]]
	    return -code error "wrong # args, should be\
                                \"$cmd ?-option value?...\"" \
		-errorcode [list TDBC GENERAL_ERROR HY000 SQLITE3 WRONGNUMARGS]
	}

	# Set one or more options

	foreach {option value} $args {
	    switch -exact -- $option {
		-e - -en - -enc - -enco - -encod - -encodi - -encodin - 
		-encoding {
		    if {$value ne {utf-8}} {
			return -code error "-encoding not supported.\
					    SQLite3 is always Unicode." \
			    -errorcode [list TDBC FEATURE_NOT_SUPPORTED 0A000 \
					    SQLITE3 ENCODING]
		    }
		}
		-i - -is - -iso - -isol - -isola - -isolat - -isolati -
		-isolatio - -isolation {
		    switch -exact -- $value {
			readu - readun - readunc - readunco - readuncom -
			readuncomm - readuncommi - readuncommit - 
			readuncommitt - readuncommitte - readuncommitted {
			    db eval {PRAGMA read_uncommitted = 1}
			}
			readc - readco - readcom - readcomm - readcommi -
			readcommit - readcommitt - readcommitte -
			readcommitted -
			rep - repe - repea - repeat - repeata - repeatab -
			repeatabl - repeatable - repeatabler - repeatablere -
			repeatablerea - repeatablread -
			s - se - ser - seri - seria - serial - seriali -
			serializ - serializa - serializab - serializabl -
			serializable -
			reado - readon - readonl - readonly {
			    db eval {PRAGMA read_uncommitted = 0}
			}
			default {
			    return -code error "bad isolation level \"$value\":\
                                should be readuncommitted, readcommitted,\
                                repeatableread, serializable, or readonly" \
				-errorcode [list TDBC GENERAL_ERROR HY000 \
						SQLITE3 BADISOLATION $value]
			}
		    }
		}
		-r - -re - -rea - -read - -reado - -readon - -readonl -
		-readonly {
		    if {$value} {
			return -code error "SQLite3's Tcl API does not support\
					    read-only access" \
			    -errorcode [list TDBC FEATURE_NOT_SUPPORTED 0A000 \
					    SQLITE3 READONLY]
		    }
		}
		-t - -ti - -tim - -time - -timeo - -timeou - -timeout {
		    if {![string is integer $value]} {
			return -code error "expected integer but got \"$value\"" \
			    -errorcode [list TDBC DATA_EXCEPTION 22018 \
					    SQLITE3 $value]
		    }
		    db timeout $value
		    set timeout $value
		}
		default {
		    return -code error "bad option \"$option\": must be\
                                        -encoding, -isolation, -readonly\
                                        or -timeout" \
			-errorcode [list TDBC GENERAL_ERROR HY000 \
					SQLITE3 BADOPTION $value]

		}
	    }
	}
	return
    }

    # The 'tables' method introspects on the tables in the database.

    method tables {{pattern %}} {
	set retval {}
	my foreach row {
	    SELECT * from sqlite_master
	    WHERE type IN ('table', 'view')
	    AND name LIKE :pattern
	} {
	    dict set row name [string tolower [dict get $row name]]
	    dict set retval [dict get $row name] $row
	}
	return $retval
    }

    # The 'columns' method introspects on columns of a table.

    method columns {table {pattern %}} {
	regsub -all ' $table '' table
	set retval {}
	set pattern [string map [list \
				     * {[*]} \
				     ? {[?]} \
				     \[ \\\[ \
				     \] \\\[ \
				     _ ? \
				     % *] [string tolower $pattern]]
	my foreach origrow "PRAGMA table_info('$table')" {
	    set row {}
	    dict for {key value} $origrow {
		dict set row [string tolower $key] $value
	    }
	    dict set row name [string tolower [dict get $row name]]
	    if {![string match $pattern [dict get $row name]]} {
		continue
	    }
	    switch -regexp -matchvar info [dict get $row type] {
		{^(.+)\(\s*([[:digit:]]+)\s*,\s*([[:digit:]]+)\s*\)\s*$} {
		    dict set row type [string tolower [lindex $info 1]]
		    dict set row precision [lindex $info 2]
		    dict set row scale [lindex $info 3]
		}
		{^(.+)\(\s*([[:digit:]]+)\s*\)\s*$} {
		    dict set row type [string tolower [lindex $info 1]]
		    dict set row precision [lindex $info 2]
		    dict set row scale 0
		}
		default {
		    dict set row type [string tolower [dict get $row type]]
		    dict set row precision 0
		    dict set row scale 0
		}
	    }
	    dict set row nullable [expr {![dict get $row notnull]}]
	    dict set retval [dict get $row name] $row
	}
	return $retval
    }

    # The 'preparecall' method prepares a call to a stored procedure.
    # SQLite3 does not have stored procedures, since it's an in-process
    # server.

    method preparecall {call} {
	return -code error {SQLite3 does not support stored procedures} \
	    -errorcode [list TDBC FEATURE_NOT_SUPPORTED 0A000 \
			    SQLITE3 PREPARECALL]
    }

    # The 'begintransaction' method launches a database transaction

    method begintransaction {} {
	db eval {BEGIN TRANSACTION}
    }

    # The 'commit' method commits a database transaction

    method commit {} {
	db eval {COMMIT}
    }

    # The 'rollback' method abandons a database transaction

    method rollback {} {
	db eval {ROLLBACK}
    }

    # The 'transaction' method executes a script as a single transaction.
    # We override the 'transaction' method of the base class, since SQLite3
    # has a faster implementation of the same thing. (The base class's generic
    # method should also work.) 
    # (Don't overload the base class method, because 'break', 'continue'
    # and 'return' in the transaction body don't work!)

    #method transaction {script} {
    #	uplevel 1 [list {*}[namespace code db] transaction $script]
    #}

    method prepare {sqlCode} {
	set result [next $sqlCode]
	return $result
    }
	
    method getDBhandle {} {
	return [namespace which db]
    }
}

#------------------------------------------------------------------------------
#
# tdbc::sqlite3::statement --
#
#	Class representing a statement to execute against a SQLite3 database
#
#------------------------------------------------------------------------------

::oo::class create ::tdbc::sqlite3::statement {

    superclass ::tdbc::statement

    # The constructor accepts the handle to the connection and the SQL
    # code for the statement to prepare.  All that it does is to parse the
    # statement and store it.  The parse is used to support the 
    # 'params' and 'paramtype' methods.

    constructor {connection sqlcode} {
	next
	my variable resultSetClass
	set resultSetClass ::tdbc::sqlite3::resultset
	my variable Params
	set Params {}
	my variable db
	set db [$connection getDBhandle]
	my variable sql
	set sql $sqlcode
	foreach token [::tdbc::tokenize $sqlcode] {
	    if {[string index $token 0] in {$ : @}} {
		dict set Params [string range $token 1 end] \
		    {type Tcl_Obj precision 0 scale 0 nullable 1 direction in}
	    }
	}
    }

    # The 'params' method returns descriptions of the parameters accepted
    # by the statement

    method params {} {
	my variable Params
	return $Params
    }

    # The 'paramtype' method need do nothing; Sqlite3 uses manifest typing.

    method paramtype args {;}

    method getDBhandle {} {
	my variable db
	return $db
    }

    method getSql {} {
	my variable sql
	return $sql
    }

}

#-------------------------------------------------------------------------------
#
# tdbc::sqlite3::resultset --
#
#	Class that represents a SQLlite result set in Tcl
#
#-------------------------------------------------------------------------------

::oo::class create ::tdbc::sqlite3::resultset {

    superclass ::tdbc::resultset

    constructor {statement args} {
	# TODO - Consider deferring running the query until the
	#        caller actually does 'nextrow' or 'foreach' - so that
	#        we know which, and can avoid the strange unpacking of
	#        data that happens in RunQuery in the 'foreach' case.

	next
	my variable db
	set db [$statement getDBhandle]
	my variable sql
	set sql [$statement getSql]
	my variable resultArray
	my variable columns
	set columns {}
	my variable results
	set results {}
	if {[llength $args] == 0} {
	    # Variable substitutions evaluated in caller's context
	    uplevel 1 [list $db eval $sql \
			   [namespace which -variable resultArray] \
			   [namespace code {my RecordResult}]]
	} elseif {[llength $args] == 1} {
	    # Variable substitutions are in the dictionary at [lindex $args 0].
	    # We have to move over into a different proc to get rid of the
	    # 'resultArray' alias in the current callframe
	    my variable paramDict
	    set paramDict [lindex $args 0]
	    my RunQuery
	} else {
	    return -code error "wrong # args: should be\
               [lrange [info level 0] 0 1] statement ?dictionary?" \
		-errorcode [list TDBC GENERAL_ERROR HY000 SQLITE3 WRONGNUMARGS]

	}
	my variable RowCount
	set RowCount [$db changes]
	my variable Cursor
	set Cursor -1
    }

    # RunQuery runs the query against the database. This procedure can have
    # no local variables, because they can suffer name conflicts with 
    # variables in the substituents of the query.  It therefore makes
    # method calls to get the SQL statement to execute and the name of the
    # result array (which is a fully qualified name).

    method RunQuery {} {
	dict with [my ParamDictName] {
	    [my getDBhandle] eval [my GetSql] [my ResultArrayName] {
		my RecordResult
	    }
	}
    }

    # Return the fully qualified name of the dictionary containing the 
    # parameters.

    method ParamDictName {} {
	my variable paramDict
	return [namespace which -variable paramDict]
    }

    # Return the SQL code to execute.

    method GetSql {} {
	my variable sql
	return $sql
    }

    # Return the fully qualified name of an array that will hold a row of
    # results from a query

    method ResultArrayName {} {
	my variable resultArray
	return [namespace which -variable resultArray]
    }

    # Record one row of results from a query by appending it as a dictionary
    # to the 'results' list.  As a side effect, set 'columns' to a list
    # comprising the names of the columns of the result.

    method RecordResult {} {
	my variable resultArray
	my variable results
	my variable columns
	set columns $resultArray(*)
	set dict {}
	foreach key $columns {
	    if {$resultArray($key) ne "\ufffd"} {
		dict set dict $key $resultArray($key)
	    }
	}
	lappend results $dict
    }

    method getDBhandle {} {
	my variable db
	return $db
    }

    # Return a list of the columns

    method columns {} {
	my variable columns
	return $columns
    }

    # Return the next row of the result set as a list

    method nextlist var {

	my variable Cursor
	my variable results

	upvar 1 $var row

	if {[incr Cursor] >= [llength $results]} {
	    return 0
	} else {
	    my variable columns
	    set row {}
	    set d [lindex $results $Cursor]
	    foreach key $columns {
		if {[dict exists $d $key]} {
		    lappend row [dict get $d $key]
		} else {
		    lappend row {}
		}
	    }
	}
	return 1
    }

    # Return the next row of the result set as a dict

    method nextdict var {

	my variable Cursor
	my variable results

	upvar 1 $var row

	if {[incr Cursor] >= [llength $results]} {
	    return 0
	} else {
	    set row [lindex $results $Cursor]
	}
	return 1
    }

    # Return the number of rows affected by a statement

    method rowcount {} {
	my variable RowCount
	return $RowCount
    }
}
