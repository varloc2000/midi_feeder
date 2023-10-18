#! /usr/bin/env tclsh
# An example of how to serve dynamic web content with
# multipart/x-mixed-replace.
# Copyright (c) 2021 D. Bohdan.  License: MIT.

package require Tcl 8.6-10

set image [join {
    data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgBAMAAACBVGfHAAAAMFBM
    VEXy9PH+/xb/ysn/ev8A+vmhk/+XmJYA6wCBgYD/R0UArQA1ZiodCuEAMQAAAXEFCARuxRLHAA
    AAg0lEQVR42r3OR1UFURAE0DuFAhz0eQZIGpCAOL4EsDAWMEB2MArI6Z3TK1Z/eztULW/YYXtk
    9focAAURYPKEgATQHFQHHYDl6oYdBqm28QpxDADEQQcd5s1/e7zhHCsu9Y29wXL3bBu4xgWSYr
    STQ6NMCMQZIBSMX0kJ8CMJYwpRAMA7X8kSzYsnKHwAAAAASUVORK5CYII=
} {}]

proc main {} {
    set server [socket -server wire 8080]

    vwait ::done
    close $server
}

proc wire {conn clientAddr clientPort} {
    # It is necessary to either disable buffering or put it in line mode or
    # to flush the socket every time you send the boundary delimiter.
    chan configure $conn \
        -blocking false \
        -buffering none \
        -translation binary \

    chan event $conn readable [list coroutine coro-$conn serve $conn]
}

proc serve conn {
    chan event $conn readable {}
    set task {}

    try {
        set path [process-request $conn]
        if {$path eq {}} return

        set boundary boundary-[expr rand()]
        send {HTTP/1.1 200 OK}
        send "Content-Type:\
            multipart/x-mixed-replace;boundary=\"$boundary\"\r\n"
        send --$boundary

        for {set i 0} {$i < 3} {incr i} {
            send "Content-Type: text/plain\r\n"
            send {Hello,       text       }
            send --$boundary
            set task [after 500 [info coroutine]]
            yield

            send "Content-Type: text/plain\r\n"
            send {       plain      world!}
            send --$boundary
            set task [after 500 [info coroutine]]
            yield
        }

        send "Content-Type: image/png\r\n"
        set imageBin [binary decode base64 [string range $::image 22 end]]
        send $imageBin false
        send --$boundary
        set task [after 1000 [info coroutine]]
        yield

        set s {Hello, the <i>wonderful</i> world of HTML!}
        set len [llength $s]
        for {set i -1} {$i <= $len} {incr i} {
            send "Content-Type: text/html\r\n"
            send "<!doctype html><h1>[lrange $s 0 $i]</h1>"

            if {$i == $len} {
                send "<img src=\"$::image\">--$boundary--"
            } else {
                send --$boundary
            }

            set task [after 300 [info coroutine]]
            yield
        }
    } trap {POSIX EPIPE} {} {
        return
    } finally {
        after cancel $task
        close $conn
    }
}

proc process-request conn {
    if {![regexp {GET (/[^ ]*) HTTP} [read $conn] _ path]} {
        send [status-phrase-response 400 {Bad Request}]
        return
    }

    if {$path ne {/}} {
        if {$path eq {/quit}} {
            send [status-phrase-response 202 Accepted]
            set ::done true
        } else {
            send [status-phrase-response 404 {Not Found}]
        }

        return
    }

    return $path
}

proc send {data {nl true}} {
    upvar 1 conn conn

    puts -nonewline $conn $data
    if {$nl} {
        puts -nonewline $conn \r\n
    }
}

proc status-phrase-response {code phrase} {
    append h "HTTP/1.1 $code $phrase\r\n"
    append h "Content-Type: text/plain\r\n\r\n$phrase."
}

main