proc walk {dir callback} {
    set callback [lreplace $callback 0 0 \
		      [uplevel 1 [list namespace which [lindex $callback 0]]]]
    foreach path [glob -nocomplain -- [file join $dir *]] {
	if {[file isdirectory $path]} {
	    walk $path $callback
	} else {
	    {*}$callback $path
	}
    }
}

proc doit {file} {
    set known 0
    set comp [file tail $file]
    foreach pattern {
	_FOSSIL_
	manifest
	manifest.uuid
	nmakehlp.c
	*.vc
	*.rc
	*.mdb
    } {
	if {[string match $pattern $comp]} {
	    return
	}
    }
    foreach pattern {
	*~
	*.log
    } {
	if {[string match $pattern $comp]} {
	    puts "cleaning up: $file"
	    file delete $file
	    return
	}
    }

    foreach pattern {
	*.3
	*.c
	*.decls
	*.h
	*.in
	*.m4
	*.n
	*.sh
	*.tcl
	*.terms
	*.test
	*.txt
	ChangeLog
	configure
	install-sh
	README
	TODO
    } {
	if {[string match $pattern $comp]} {
	    set f [open $file r]
	    chan configure $f -translation lf
	    set data [read $f]
	    close $f
	    if {[regsub -all {\r+\n} $data \n data]} {
		puts "converting from DOS format: $file"
		set f [open $file w]
		chan configure $f -translation lf
		puts -nonewline $f $data
		close $f
	    }
	    return
	}
    }
    puts "Ignoring: $file"
}

walk . doit


	    
	return
    }
    puts "Ignoring: $file"
}

walk . doit


	    

	      