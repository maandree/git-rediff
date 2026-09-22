set -eu

tmp=testdir.$$
command="../git-rediff"

interactive () {
	test $# = 0
	command="../git-rediff.t"
}

cleanup () {
	cd -- ..
	rm -rf -- "${tmp}"
}

trap cleanup 0 HUP INT TERM

mkdir -- "${tmp}"
cd -- "${tmp}"
git init -q .
git config user.email test@example.invalid
git config user.name test

set_marker_variables () {
	test $# = 1
	HEAD="$(printf '%*.s\n' "$1" '' | tr ' ' '<')"
	PARENT="$(printf '%*.s\n' "$1" '' | tr ' ' '|')"
	TAIL="$(printf '%*.s\n' "$1" '' | tr ' ' '=')"
	END="$(printf '%*.s\n' "$1" '' | tr ' ' '>')"
}

set_marker_variables 7

set_conflict_marker_size () {
	test $# = 2
	# $1 = marker size
	# $2 = affected file
	set_marker_variables "$1"
	printf '%s conflict-marker-size=%s\n' "$2" "$1" >> .gitattributes
}

again_command=''

save_files () {
	while test ! $# = 0; do
		if test "$1" = - || test "$1" = --; then
			shift 1
			break
		elif printf '%s\n' "$1" | grep -q '^-'; then
			shift 1
		else
			break
		fi
	done
	for f; do
		if test ! "$f" = -; then
			cp -- "$f" "$f".original
		fi
	done
}

no_error () {
	test -z "${again_command}"
	again_command="$*"
	shift 1
	save_files "$@"
	set +e
	"$command" "$@"
	status=$?
	set -e
	while test ! $# = 0; do
		if test "$1" = - || test "$1" = --; then
			shift 1
			break
		elif printf '%s\n' "$1" | grep -q '^-'; then
			shift 1
		else
			break
		fi
	done
	for f; do
		if test ! "$f" = -; then
			diff -u -- "$f" "$f".expected >&2
			cp -- "$f".original "$f"
		fi
	done
}

normal () {
	no_error normal "$@"
	test $status = 0
}

conflict () {
	no_error conflict "$@"
	test $status = 1
}

error () {
	set +e
	"$command" "$@" 2>/dev/null
	status=$?
	set -e
	test $status = 2
}

also_interactive () {
	test $# = 1
	test -n "${again_command}"
	cmd="${again_command}"
	again_command=''
	interactive
	printf '%s' "$1" > i-input
	${cmd} 8<i-input 9>/dev/null
}

interactive_line () {
	test $# = 2
	printf '\e[%sm%s\n\e[0m' "$1" "$2"
}

interactive_marker () {
	test $# = 1
	interactive_line 36 "$1"
}

interactive_side () {
	test $# = 1
	interactive_line 32 "$1"
}

interactive_base () {
	test $# = 1
	interactive_line 31 "$1"
}

interactive_filename () {
	test $# = 1
	printf '\e[1m%s\e[0m\n' "$1"
}

interactive_prompt () {
	test $# = 2
	printf '\e[1;34m(%s/%s) Resolve this conflict [h,t,b,m,r,e,s,d,q,?]? \e[0m\n' "$1" "$2"
}

interactive_help () {
	test $# = 1
	printf '\e[1;31m'
	printf '%s\n' \
		'h - select head' \
		't - select tail'
	if test "$1" = 3; then
		printf '%s\n' 'b - remove base'
	fi
	printf '%s\n' \
		'm - merge' \
		'r - reduce' \
		'e - manually edit this hunk' \
		's - skip this hunk' \
		'd - skip this hunk and all later hunks in the file' \
		'q - quit; skip this hunk and all remaining hunks' \
		'? - print help'
	printf '\e[0m'
}
