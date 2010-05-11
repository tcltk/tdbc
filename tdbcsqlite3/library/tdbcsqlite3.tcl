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

package provide tdbc::sqlite3 1.0b15

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

    variable timeout

    # The constructor accepts a database name and opens the database.

    constructor {databaseName args} {
	set timeout 0
	if {[llength $args] % 2 != 0} {
	    set cmd [lrange [info level 0] 0 end-[llength $args]]
	    return -code error \
		-errorcode {TDBC GENERAL_ERROR HY000 SQLITE3 WRONGNUMARGS} \
		"wrong # args, should be \"$cmd ?-option value?...\""
	}
	next
	sqlite3 [namespace current]::db $databaseName
	if {[llength $args] > 0} {
	    my configure {*}$args
	}
	db nullvalue \ufffd
    }

    # The 'statementCreate' method forwards to the constructor of the
    # statement class

    forward statementCreate ::tdbc::sqlite3::statement create

    # The 'configure' method queries and sets options to the database

    method configure args {
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
		    return -code error \
			-errorcode [list TDBC GENERAL_ERROR HY000 SQLITE3 \
					BADOPTION $option] \
			"bad option \"$option\": must be\
                         -encoding, -isolation, -readonly or -timeout"
		    
		}
	    }

	} elseif {[llength $args] % 2 != 0} {

	    # Syntax error

	    set cmd [lrange [info level 0] 0 end-[llength $args]]
	    return -code error \
		-errorcode [list TDBC GENERAL_ERROR HY000 \
				SQLITE3 WRONGNUMARGS] \
		"wrong # args, should be \" $cmd ?-option value?...\""
	}

	# Set one or more options

	foreach {option value} $args {
	    switch -exact -- $option {
		-e - -en - -enc - -enco - -encod - -encodi - -encodin - 
		-encoding {
		    if {$value ne {utf-8}} {
			return -code error \
			    -errorcode [list TDBC FEATURE_NOT_SUPPORTED 0A000 \
					    SQLITE3 ENCODING] \
			    "-encoding not supported. SQLite3 is always \
                             Unicode."
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
			    return -code error \
				-errorcode [list TDBC GENERAL_ERROR HY000 \
						SQLITE3 BADISOLATION $value] \
				"bad isolation level \"$value\":\
                                should be readuncommitted, readcommitted,\
                                repeatableread, serializable, or readonly"
			}
		    }
		}
		-r - -re - -rea - -read - -reado - -readon - -readonl -
		-readonly {
		    if {$value} {
			return -code error \
			    -errorcode [list TDBC FEATURE_NOT_SUPPORTED 0A000 \
					    SQLITE3 READONLY] \
			    "SQLite3's Tcl API does not support read-only\
                             access"
		    }
		}
		-t - -ti - -tim - -time - -timeo - -timeou - -timeout {
		    if {![string is integer $value]} {
			return -code error \
			    -errorcode [list TDBC DATA_EXCEPTION 22018 \
					    SQLITE3 $value] \
			    "expected integer but got \"$value\""
		    }
		    db timeout $value
		    set timeout $value
		}
		default {
		    return -code error \
			-errorcode [list TDBC GENERAL_ERROR HY000 \
					SQLITE3 BADOPTION $value] \
			"bad option \"$option\": must be\
                         -encoding, -isolation, -readonly or -timeout"

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
	return -code error \
	    -errorcode [list TDBC FEATURE_NOT_SUPPORTED 0A000 \
			    SQLITE3 PREPARECALL] \
	    {SQLite3 does not support stored procedures}
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

    variable Params db sql

    # The constructor accepts the handle to the connection and the SQL
    # code for the statement to prepare.  All that it does is to parse the
    # statement and store it.  The parse is used to support the 
    # 'params' and 'paramtype' methods.

    constructor {connection sqlcode} {
	next
	set Params {}
	set db [$connection getDBhandle]
	set sql $sqlcode
	foreach token [::tdbc::tokenize $sqlcode] {
	    if {[string index $token 0] in {$ : @}} {
		dict set Params [string range $token 1 end] \
		    {type Tcl_Obj precision 0 scale 0 nullable 1 direction in}
	    }
	}
    }

    # The 'resultSetCreate' method relays to the result set constructor

    forward resultSetCreate ::tdbc::sqlite3::resultset create

    # The 'params' method returns descriptions of the parameters accepted
    # by the statement

    method params {} {
	return $Params
    }

    # The 'paramtype' method need do nothing; Sqlite3 uses manifest typing.

    method paramtype args {;}

    method getDBhandle {} {
	return $db
    }

    method getSql {} {
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

    # The variables of this class all have peculiar names. The reason is
    # that the RunQuery method needs to execute with an activation record
    # that has no local variables whose names could conflict with names
    # in the SQL query. We start the variable names with hyphens because
    # they can't be bind variables.

    variable -columns -db -resultArray -results -sql -Cursor -RowCount

    constructor {statement args} {
	next
	set -db [$statement getDBhandle]
	set -sql [$statement getSql]
	set -columns {}
	set -results {}
	if {[llength $args] == 0} {

	    # Variable substitutions are evaluated in caller's context

	    uplevel 1 [list ${-db} eval ${-sql} \
			   [namespace which -variable -resultArray] \
			   [namespace code {my RecordResult}]]

	} elseif {[llength $args] == 1} {

	    # Variable substitutions are in the dictionary at [lindex $args 0].

	    set -paramDict [lindex $args 0]

	    # At this point, the activation record must contain no variables
	    # that might be bound within the query.  All variables at this point
	    # begin with hyphens so that they are syntactically incorrect
	    # as bound variables in SQL.

	    unset args
	    unset statement

	    dict with -paramDict {
		${-db} eval ${-sql} -resultArray {
		    my RecordResult
		}
	    }

	} else {

	    # Too many args

	    return -code error \
		-errorcode [list TDBC GENERAL_ERROR HY000 \
				SQLITE3 WRONGNUMARGS] \
		"wrong # args: should be\
                 [lrange [info level 0] 0 1] statement ?dictionary?"

	}
	set -RowCount [${-db} changes]
	set -Cursor -1
    }

    # Record one row of results from a query by appending it as a dictionary
    # to the 'results' list.  As a side effect, set 'columns' to a list
    # comprising the names of the columns of the result.

    method RecordResult {} {
	set -columns ${-resultArray(*)}
	set dict {}
	foreach key ${-columns} {
	    if {[set -resultArray($key)] ne "\ufffd"} {
		dict set dict $key [set -resultArray($key)]
	    }
	}
	lappend -results $dict
    }

    method getDBhandle {} {
	return ${-db}
    }

    # Return a list of the columns

    method columns {} {
	return ${-columns}
    }

    # Return the next row of the result set as a list

    method nextlist var {

	upvar 1 $var row

	if {[incr -Cursor] >= [llength ${-results}]} {
	    return 0
	} else {
	    set row {}
	    set d [lindex ${-results} ${-Cursor}]
	    foreach key ${-columns} {
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

	upvar 1 $var row

	if {[incr -Cursor] >= [llength ${-results}]} {
	    return 0
	} else {
	    set row [lindex ${-results} ${-Cursor}]
	}
	return 1
    }

    # Return the number of rows affected by a statement

    method rowcount {} {
	return ${-RowCount}
    }
}
