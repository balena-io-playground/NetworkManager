#!/bin/bash


die() {
    echo "$*" >&2
    exit 1
}

usage() {
    echo "USAGE: $0 [-h|--help|-?|help]"
    echo
    echo "Does all the steps from a clean working directory to an RPM of NetworkManager"
    echo
}


ORIGDIR="$(readlink -f "$PWD")"
SCRIPTDIR="$(dirname "$(readlink -f "$0")")"
GITDIR="$(cd "$SCRIPTDIR" && git rev-parse --show-toplevel || die "Could not get GITDIR")"


[[ -x "$SCRIPTDIR"/build.sh ]] || die "could not find \"$SCRIPTDIR/build.sh\""

cd "$GITDIR" || die "could not change to $GITDIR"

IGNORE_DIRTY=0
GIT_CLEAN=0

for A; do
    case "$A" in
        -h|--help|-\?|help)
            usage
            exit 0
            ;;
        -f|--force)
            IGNORE_DIRTY=1
            ;;
        -c|--clean)
            GIT_CLEAN=1
            ;;
        *)
            usage
            die "Unexpected argument \"$A\""
            ;;
    esac
done

if [[ $GIT_CLEAN == 1 ]]; then
    git clean -fdx :/
fi

if [[ $IGNORE_DIRTY != 1 ]]; then
    # check for a clean working directory.
    # We ignore the /contrib directory, because this is where the automation
    # scripts and the build results will be.
    if [[ "x$(git clean -ndx | grep '^Would remove contrib/.*$' -v)" != x ]]; then
        die "The working directory is not clean. Refuse to run. Try    git clean -e /contrib -dx -n"
    fi
    if [[ "x$(git status --porcelain)" != x ]]; then
        die "The working directory has local changes. Refuse to run. Try $0 --force"
    fi
fi

./autogen.sh --enable-gtk-doc || die "Error autogen.sh"

make -j 10 || die "Error make"

make distcheck || die "Error make distcheck"

"$SCRIPTDIR"/build.sh

