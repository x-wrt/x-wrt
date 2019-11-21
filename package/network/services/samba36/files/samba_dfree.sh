#!/bin/sh
__tol=0
__free=0
out=
if [ "`pwd`" = "/mnt" ] && [ "$1" = "." ] || [ "$1" = "/mnt" ]; then
	out=`for sd in /mnt/*; do
		df $sd 2>/dev/null | tail -1
	done | grep "^/dev" | awk '{print $2" "$4}' | while read _tol _free; do
		__tol=$((__tol+_tol))
		__free=$((__free+_free))
		echo $__tol $__free
	done | tail -n1`
fi

test -n "$out" && echo "$out" || \
df $1 | tail -1 | awk '{print $2" "$4}'
