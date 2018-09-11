#!/bin/sh

export QTDIR=$PWD

export OPIEDIR=$PWD/../opie

export LD_LIBRARY_PATH=$OPIEDIR/lib:$QTDIR/lib:$LD_LIBRARY_PATH

cd $QTDIR
echo 'yes' | ./configure -qconfig qpe -depths 4,16,24,32 -system-jpeg -system-libpng -system-zlib -no-xkb -no-sm -no-xft -qvfb
make
