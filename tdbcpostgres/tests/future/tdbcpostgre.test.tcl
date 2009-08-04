
#this test was abandonded, because no flags are used as for now
test tdbc::postgres-1.4 {create a connection, bad flag} {*}{
    -body {
	tdbc::postgres::connection create db -interactive rubbish
    }
    -returnCodes error
    -result {expected boolean value but got "rubbish"}
}


#Theese two test are delayed, cause only varchar type is implemented as for now
test tdbc::postgres-5.4 {paramtype - bad scale} {*}{
    -setup {
	set stmt [::db prepare {
	    INSERT INTO people(idnum, name, info) values(:idnum, :name, 0)
	}]
    }
    -body {
	$stmt paramtype idnum decimal rubbish
    }
    -cleanup {
	rename $stmt {}
    }
    -returnCodes error
    -match glob
    -result {expected integer but got "rubbish"}
}

test tdbc::postgres-5.5 {paramtype - bad precision} {*}{
    -setup {
	set stmt [::db prepare {
	    INSERT INTO people(idnum, name, info) values(:idnum, :name, 0)
	}]
    }
    -body {
	$stmt paramtype idnum decimal 12 rubbish
    }
    -cleanup {
	rename $stmt {}
    }
    -returnCodes error
    -match glob
    -result {expected integer but got "rubbish"}
}



############### FUTURE tests:


test tdbc::postgres-19.5 {$connection configure - set inappropriate arg} {*}{
    -body {
	list [catch {::db configure -encoding ebcdic} result] \
	    $result $::errorCode
    }
    -result {1 {"-encoding" option cannot be changed dynamically} {TDBC GENERAL_ERROR HY000 POSTGRES -1}}
}

test tdbc::postgres-19.6 {$connection configure - wrong # args} {*}{
    -body {
	::db configure -parent . -junk
    }
    -returnCodes error
    -match glob
    -result "wrong # args*"
}

test tdbc::postgres-19.9 {$connection configure - -encoding} {*}{
    -body {
	::db configure -encoding
    }
    -result utf-8
}


test tdbc::postgres-19.10 {$connection configure - -isolation} {*}{
    -body {
	::db configure -isolation junk
    }
    -returnCodes error
    -match glob
    -result {bad isolation level "junk"*}
}

test tdbc::postgres-19.11 {$connection configure - -isolation} {*}{
    -body {
	list [::db configure -isolation readuncommitted] \
	    [::db configure -isolation] \
	    [::db configure -isolation readcommitted] \
	    [::db configure -isolation] \
	    [::db configure -isolation serializable] \
	    [::db configure -isolation] \
	    [::db configure -isolation repeatableread] \
	    [::db configure -isolation]
    }
    -result {{} readuncommitted {} readcommitted {} serializable {} repeatableread}
}

test tdbc::postgres-19.12 {$connection configure - -readonly} {*}{
    -body {
	::db configure -readonly junk
    }
    -returnCodes error
    -result {"-readonly" option cannot be changed dynamically}
}

test tdbc::postgres-19.13 {$connection configure - -readonly} {*}{
    -body {
	::db configure -readonly
    }
    -result 0
}

test tdbc::postgres-19.14 {$connection configure - -timeout} {*}{
    -body {
	::db configure -timeout junk
    }
    -returnCodes error
    -result {expected integer but got "junk"}
}

test tdbc::postgres-19.15 {$connection configure - -timeout} {*}{
    -body {
	set x [::db configure -timeout]
	list [::db configure -timeout 5000] [::db configure -timeout] \
	    [::db configure -timeout $x]
    }
    -result {{} 5000 {}}
}

test tdbc::postgres-19.16 {$connection configure - -db} {*}{
    -body {
	set x [::db configure -db]
	list [::db configure -db information_schema] \
	    [::db configure -db] \
	    [::db configure -db $x]
    }
    -result {{} information_schema {}}
}

test tdbc::postgres-19.17 {$connection configure - -user} \
    -body {
	set flags $::connFlags
	dict unset flags -host
	catch [dict unset flags -port]
	catch [dict unset flags -socket]
	set flags2 $flags
	dict set flags -db information_schema
	list [::db configure {*}$flags] [::db configure -db] \
	    [::db configure {*}$flags2] [::db configure -db]
    } \
    -result [list {} information_schema {} [dict get $connFlags -db]]

test tdbc::postgres-20.1 {bit values} {*}{
    -setup {
	catch {db allrows {DROP TABLE bittest}}
	db allrows {
	    CREATE TABLE bittest (
		bitstring BIT(14)
	    )
	}
	db allrows {INSERT INTO bittest(bitstring) VALUES(b'11010001010110')}
    }
    -body {
	db allrows {SELECT bitstring FROM bittest}
    }
    -result {{bitstring 13398}}
    -cleanup {
	db allrows {DROP TABLE bittest}
    }
}

test tdbc::postgres-20.2 {direct value transfers} {*}{
    -setup {
	set bigtext [string repeat a 200]
	set bigbinary [string repeat \xc2\xa1 100]
	catch {db allrows {DROP TABLE typetest}}
	db allrows {
	    CREATE TABLE typetest (
		xtiny1 TINYINT,
		xsmall1 SMALLINT,
		xint1 INTEGER,
		xfloat1 FLOAT,
		xdouble1 DOUBLE,
		xtimestamp1 TIMESTAMP,
		xbig1 BIGINT,
		xmed1 MEDIUMINT,
		xdate1 DATE,
		xtime1 TIME,
		xdatetime1 DATETIME,
		xyear1 YEAR,
		xbit1 BIT(14),
		xdec1 DECIMAL(10),
		xtinyt1 TINYTEXT,
		xtinyb1 TINYBLOB,
		xmedt1 MEDIUMTEXT,
		xmedb1 MEDIUMBLOB,
		xlongt1 LONGTEXT,
		xlongb1 LONGBLOB,
		xtext1 TEXT,
		xblob1 BLOB,
		xvarb1 VARBINARY(256),
		xvarc1 VARCHAR(256),
		xbin1 BINARY(20),
		xchar1 CHAR(20)
	    )
	}
	set stmt [db prepare {
	    INSERT INTO typetest(
		xtiny1,		xsmall1,	xint1,		xfloat1,
		xdouble1,	xtimestamp1,	xbig1,		xmed1,
		xdate1,		xtime1,		xdatetime1,	xyear1,
		xbit1,		xdec1,		xtinyt1,	xtinyb1,
		xmedt1,		xmedb1,		xlongt1,	xlongb1,
		xtext1,		xblob1,		xvarb1,		xvarc1,
		xbin1,		xchar1
	    ) values (
		:xtiny1,	:xsmall1,	:xint1,		:xfloat1,
		:xdouble1,	:xtimestamp1,	:xbig1,		:xmed1,
		:xdate1,	:xtime1,	:xdatetime1,	:xyear1,
		:xbit1,		:xdec1,		:xtinyt1,	:xtinyb1,
		:xmedt1,	:xmedb1,	:xlongt1,	:xlongb1,
		:xtext1,	:xblob1,	:xvarb1,	:xvarc1,
		:xbin1,		:xchar1
	    )
	}]
	$stmt paramtype xtiny1 tinyint
	$stmt paramtype xsmall1 smallint
	$stmt paramtype xint1 integer
	$stmt paramtype xfloat1 float
	$stmt paramtype xdouble1 double
	$stmt paramtype xtimestamp1 timestamp
	$stmt paramtype xbig1 bigint
	$stmt paramtype xmed1 mediumint
	$stmt paramtype xdate1 date
	$stmt paramtype xtime1 time
	$stmt paramtype xdatetime1 datetime
	$stmt paramtype xyear1 year
	$stmt paramtype xbit1 bit 14
	$stmt paramtype xdec1 decimal 10 0
	$stmt paramtype xtinyt1 tinytext
	$stmt paramtype xtinyb1 tinyblob
	$stmt paramtype xmedt1 mediumtext
	$stmt paramtype xmedb1 mediumblob
	$stmt paramtype xlongt1 longtext
	$stmt paramtype xlongb1 longblob
	$stmt paramtype xtext1 text
	$stmt paramtype xblob1 blob
	$stmt paramtype xvarb1 varbinary
	$stmt paramtype xvarc1 varchar
	$stmt paramtype xbin1 binary 20
	$stmt paramtype xchar1 char 20
    } 
    -body {
	set trouble {}
	set xtiny1 0x14
	set xsmall1 0x3039
	set xint1 0xbc614e
	set xfloat1 1.125
	set xdouble1 1.125
	set xtimestamp1 {2001-02-03 04:05:06}
	set xbig1 0xbc614e
	set xmed1 0x3039
	set xdate1 2001-02-03
	set xtime1 04:05:06
	set xdatetime1 {2001-02-03 04:05:06}
	set xyear1 2001
	set xbit1 0b11010001010110
	set xdec1 0xbc614e
	set xtinyt1 $bigtext
	set xtinyb1 $bigbinary
	set xmedt1 $bigtext
	set xmedb1 $bigbinary
	set xlongt1 $bigtext
	set xlongb1 $bigbinary
	set xtext1 $bigtext
	set xblob1 $bigbinary
	set xvarb1 $bigbinary
	set xvarc1 $bigtext
	set xbin1 [string repeat \xc2\xa1 10]
	set xchar1 [string repeat a 20]
	$stmt allrows
	db foreach row {select * from typetest} {
	    foreach v {
		xtiny1		xsmall1		xint1		xfloat1
		xdouble1	xtimestamp1	xbig1		xmed1
		xdate1		xtime1		xdatetime1	xyear1
		xbit1		xdec1		xtinyt1		xtinyb1
		xmedt1		xmedb1		xlongt1		xlongb1
		xtext1		xblob1		xvarb1		xvarc1
		xbin1		xchar1
	    } {
		if {![dict exists $row $v]} {
		    append trouble $v " did not appear in result set\n"
		} elseif {[set $v] != [dict get $row $v]} {
		    append trouble [list $v is [dict get $row $v] \
					should be [set $v]] \n
		}
	    }
	}
	set trouble
    }
    -result {}
    -cleanup {
	$stmt close
	db allrows {
	    DROP TABLE typetest
	}
    }
}

test tdbc::postgres-21.2 {transfers of binary data} {*}{
    -setup {
	catch {
	    db allrows {DROP TABLE bintest}
	}
	db allrows {
	    CREATE TABLE bintest (
		xint1 INTEGER PRIMARY KEY,
		xbin VARBINARY(256)
	    )
	}
	set stmt1 [db prepare {
	    INSERT INTO bintest (xint1, xbin)
	    VALUES(:i1, :b1)
	}]
	$stmt1 paramtype i1 integer
	$stmt1 paramtype b1 varbinary 256
	set stmt2 [db prepare {
	    SELECT xbin FROM bintest WHERE xint1 = :i1
	}]
	$stmt2 paramtype i1 integer
    }
    -body {
	set listdata {}
	for {set i 0} {$i < 256} {incr i} {
	    lappend listdata $i
	}
	set b1 [binary format c* $listdata]
	set i1 123
	$stmt1 allrows
	$stmt2 foreach -as lists row { set b2 [lindex $row 0] }
	list [string length $b2] [string compare $b1 $b2]
    }
    -result {256 0}
    -cleanup {
	$stmt1 close
	$stmt2 close
	db allrows {DROP TABLE bintest}
    }
}

test tdbc::postgres-22.1 {duplicate column name} {*}{
    -body {
	set stmt [::db prepare {
	    SELECT a.idnum, b.idnum 
	    FROM people a, people b
	    WHERE a.name = 'hud rockstone' 
	    AND b.info = a.info
	}]
	set rs [$stmt execute]
	$rs columns
    }
    -result {idnum idnum#2}
    -cleanup {
	$rs close
	$stmt close
    }
}

#-------------------------------------------------------------------------------

# Test cleanup. Get rid of the test database

catch {rename ::db {}}

cleanupTests
return

# Local Variables:
# mode: tcl
# End:
