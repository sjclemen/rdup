#!/bin/bash
#
# Copyright (c) 2005, 2006 Miek Gieben
# See LICENSE for the license
#
# This script implement a mirroring backup scheme
# -c is used for remote mirroring

set -o nounset

S_ISDIR=16384   # octal: 040000 (This seems to be portable...)
S_ISLNK=40960   # octal: 0120000
S_MMASK=4095    # octal: 00007777, mask to get permission
remote=0
verbose=0
idir=0; ireg=0; ilnk=0; irm=0
ftsize=0
ts=`date +%s` # gnuism
backupdir=""
PROGNAME=$0

_echo() {
        echo "** $1" 
}

_echo2() {
        echo "** $1" > /dev/fd/2
}

cleanup() {
        # can also happen when running remotely (no /dev/fd/2)
        _echo "$PROGNAME: Signal received while processing \`$path', exiting" 
        exit 1
}
# trap at least these
trap cleanup SIGINT SIGPIPE

mirror_suffix() {
        case $OSTYPE in
                linux*)
                        s=$(stat -c '%y' "$@")  
                        echo "+${s:8:2}.${s:11:5}"
                ;;
                freebsd*)
                        s=$(stat -f '%Sm' "$@")
                        # add prefix 0
                        printf "+%02d.%s" ${s:4:2} ${s:7:5}
                ;;
        esac
}

mirror_create_top() {
        # need to reverse the order
        dir=$1;
        while [[ $dir != "/" ]]
        do
                dirs="$dir $dirs"
                dir=`dirname $dir`
        done
        for d in $dirs; do
                if [[ ! -d $d ]]; then
                        mkdir -m 755 "$d"
                        chown root:backup "$d"
                fi
        done
}

usage() {
        echo "$PROGNAME [OPTIONS]"
        echo
        echo Mirror the files in the filelist from rdup
        echo
        echo OPTIONS
        echo " -c      process the file content also (rdup -c), for remote backups"
        echo " -b DIR  use DIR as the backup directory, YYYYMM will be added"
        echo " -v      echo the files processed to stderr"
        echo " -h      this help"
}

local_mirror() {
        declare -a path # catch spacing in the path
        while read mode uid gid psize fsize path
        do
                dump=${mode:0:1}                # to add or remove
                mode=${mode:1}                  # st_mode bits
                bits=$(($mode & $S_MMASK))      # permission bits
                bits=`printf "%o" $bits`        # and back to octal again
                typ=0
                if [[ $(($mode & $S_ISDIR)) == $S_ISDIR ]]; then
                        typ=1;
                fi
                if [[ $(($mode & $S_ISLNK)) == $S_ISLNK ]]; then
                        typ=2;
                fi
                
                [[ $verbose -eq 1 ]] && echo $path > /dev/fd/2

                if [[ -e "$backupdir/$path" ]]; then
                        suffix=`mirror_suffix "$backupdir/$path"`
                fi

                if [[ $dump == "+" ]]; then
                        # add
                        case $typ in
                                0)      # REG
                                if [[ -e "$backupdir/$path" ]]; then
                                        mv "$backupdir/$path" "$backupdir/$path$suffix"
                                fi
                                cat "$path" > "$backupdir/$path"
                                chown $uid:$gid "$backupdir/$path"
                                chmod $bits "$backupdir/$path"
                                ftsize=$(($ftsize + $fsize))
                                ireg=$(($ireg + 1))
                                ;;
                                1)      # DIR
                                # check for fileTYPE changes
                                if [ -f "$backupdir/$path" -o -L "$backupdir/$path" ]; then
                                        mv "$backupdir/$path" "$backupdir/$path$suffix"
                                fi

                                [[ ! -d "$backupdir/$path" ]] && mkdir -p "$backupdir/$path" 
                                chown $uid:$gid "$backupdir/$path"
                                chmod $bits "$backupdir/$path"
                                idir=$(($idir + 1))
                                ;;
                                2)      # LNK
                                if [[ -e "$backupdir/$path" ]]; then
                                        mv "$backupdir/$path" "$backupdir/$path$suffix"
                                fi
                                cp -RP "$path" "$backupdir/$path"
                                chown -h $uid:$gid "$backupdir/$path"
                                ilnk=$(($ilnk + 1))
                                ;;
                        esac
                else
                        # move. It could be the stuff is not there, don't
                        # error on that.
                        if [[ -e "$backupdir/$path" ]]; then
                                mv "$backupdir/$path" "$backupdir/$path$suffix"
                        fi
                        irm=$(($irm + 1))
                fi
        done 
        te=`date +%s`
        _echo2 "#REG FILES  : $ireg"
        _echo2 "#DIRECTORIES: $idir"
        _echo2 "#LINKS      : $ilnk"
        _echo2 "#(RE)MOVED  : $irm" 
        _echo2 "SIZE        : $(($ftsize / 1024 )) KB" 
        _echo2 "STORED IN   : $backupdir" 
        _echo2 "ELAPSED     : $(($te - $ts)) s"
}

remote_mirror() {
        while read mode uid gid psize fsize
        do
                dump=${mode:0:1}        # to add or remove
                mode=${mode:1}          # st_mode bits
                bits=$(($mode & $S_MMASK)) # permission bits
                bits=`printf "%o" $bits` # and back to octal again
                typ=0
                path=`head -c $psize`   # get the path
                if [[ $(($mode & $S_ISDIR)) == $S_ISDIR ]]; then
                        typ=1;
                fi
                if [[ $(($mode & $S_ISLNK)) == $S_ISLNK ]]; then
                        typ=2;
                fi

                # check sanity of data?

# debugging - the output of rdup should perfectly match our reads
#echo "$dump$mode $uid $gid $psize $fsize"
#                echo "m{"$mode"}"
#                echo "u{"$uid"}"
#                echo "g{"$gid"}"
#                echo "l{"$psize"}"
#                echo "s{"$fsize"}"
#                echo "p{"$path"}"

                if [[ -e "$backupdir/$path" ]]; then
                        suffix=`mirror_suffix "$backupdir/$path"`
                fi

                if [[ $dump == "+" ]]; then
                        # add
                        case $typ in
                                0)      # REG
                                if [[ -e "$backupdir/$path" ]]; then
                                        mv "$backupdir/$path" "$backupdir/$path$suffix"
                                fi
                                if [[ $fsize -ne 0 ]]; then
                                        # catch
                                        head -c $fsize > "$backupdir/$path"
                                else 
                                        # empty
                                        touch "$backupdir/$path"
                                fi
                                chown $uid:$gid "$backupdir/$path" 2>/dev/null
                                chmod $bits "$backupdir/$path" 
                                ftsize=$(($ftsize + $fsize))
                                ireg=$(( $ireg + 1))
                                ;;
                                1)      # DIR
                                # check for fileTYPE changes
                                if [ -f "$backupdir/$path" -o -L "$backupdir/$path" ]; then
                                        mv "$backupdir/$path" "$backupdir/$path$suffix"
                                fi

                                # size should be 0
                                [[ ! -d "$backupdir/$path" ]] && mkdir -p "$backupdir/$path"
                                chown $uid:$gid "$backupdir/$path" 2>/dev/null
                                chmod $bits "$backupdir/$path" 
                                idir=$(( $idir + 1))
                                ;;
                                2)      # LNK, target is in the content! 
                                if [[ -e "$backupdir/$path" ]]; then
                                        mv "$backupdir/$path" "$backupdir/$path$suffix"
                                fi
                                target=`head -c $fsize`
                                ln -sf "$target" "$backupdir/$path" 
                                chown -h $uid:$gid "$backupdir/$path" 2>/dev/null
                                ilnk=$(( $ilnk + 1))
                                ;;
                        esac
                else
                        # remove
                        if [[ -e "$backupdir/$path" ]]; then
                                mv "$backupdir/$path" "$backupdir/$path$suffix"
                        fi
                        irm=$(( $irm + 1))
                fi
        done 
        te=`date +%s`
        # /dev/fd/2 is not available remotely
        _echo "** #REG FILES  : $ireg"
        _echo "** #DIRECTORIES: $idir"
        _echo "** #LINKS      : $ilnk"
        _echo "** #(RE)MOVED  : $irm"
        _echo "** SIZE        : $(($ftsize / 1024 )) KB"
        _echo "** STORED IN   : $backupdir"
        _echo "** ELAPSED     : $(($te - $ts)) s"
}

while getopts ":cvhb:" options; do
        case $options in
                c) remote=1;;
                b) backupdir=$OPTARG;;
                v) verbose=1;;
                h) usage && exit;;
                \?) usage && exit;;
        esac
done
shift $((OPTIND - 1))

if [ -z "$backupdir" ]; then
        backupdir="/vol/backup/`hostname`"
fi
backupdir=$backupdir/`date +%Y%m`

if [[ $remote -eq 0 ]]; then
        local_mirror
else
        remote_mirror
fi
